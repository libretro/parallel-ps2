#include <string.h>
#include "common/Pcsx2Defs.h"
#include <libretro.h>

#include "../pcsx2/Patch.h"
#include "patches.h"

/* Every per-game block hands its lines to one loader. */
static void load_patch_lines(const char* const* lines, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
		LoadPatchesFromString(lines[i]);
}
#define LOAD_PATCHES(lines) load_patch_lines((lines), sizeof(lines) / sizeof((lines)[0]))

int lrps2_ingame_patches(const char *serial,
		u32 game_crc,
		const char *renderer,
		bool   hint_nointerlacing,
		bool   hint_disable_mipmaps,
		bool   hint_game_enhancements,
		int8_t hint_widescreen,
		int8_t hint_uncapped_framerate,
		int8_t hint_language_unlock,
		bool   fastcdvd)
{
	int set_system_av_info = 0;

	if (hint_nointerlacing)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* Ace Combat 04 - Shattered Skies (NTSC-U) [CRC: A32F7CD0] */
			if (!strcmp(serial, "SLUS-20152"))
			{
				/* Patch courtesy: asasega */
				static const char *const patches[] = {
					"patch=1,EE,E0050003,extended,0029F418",
					"patch=1,EE,0029F418,extended,00000001",
					"patch=1,EE,D029F420,extended,0000948C",
					"patch=1,EE,0029F420,extended,00000000",
					"patch=1,EE,D029F420,extended,00009070",
					"patch=1,EE,0029F420,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ace Combat 04 - Shattered Skies (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Ace Combat 5 - The Unsung War (NTSC-U) [CRC: 39B574F0] */
			else if (!strcmp(serial, "SLUS-20851"))
			{
				/* Patch courtesy: asasega */
				static const char *const patches[] = {
					"patch=1,EE,2032CA34,extended,0C03FFF3",
					"patch=1,EE,2032CA3C,extended,00000000",
					"patch=1,EE,200FFFCC,extended,341B0001",
					"patch=1,EE,200FFFD0,extended,147B0004",
					"patch=1,EE,200FFFD4,extended,34030001",
					"patch=1,EE,200FFFD8,extended,FC430000",
					"patch=1,EE,200FFFDC,extended,03E00008",
					"patch=1,EE,200FFFE0,extended,DE020010",
					"patch=1,EE,200FFFE4,extended,FC430000",
					"patch=1,EE,200FFFE8,extended,DE020010",
					"patch=1,EE,200FFFEC,extended,03E00008",
					"patch=1,EE,200FFFF0,extended,30429400"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ace Combat 5 - The Unsung War (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Ace Combat Zero - The Belkan War (NTSC-U) [CRC: 65729657] */
			else if (!strcmp(serial, "SLUS-21346"))
			{
				/* Patch courtesy: asasega */
				static const char *const patches[] = {
					"patch=1,EE,202F9A14,extended,24020001",
					"patch=1,EE,202F9D58,extended,0C03FFF0",
					"patch=1,EE,200FFFC0,extended,341B9070",
					"patch=1,EE,200FFFC4,extended,145B0002",
					"patch=1,EE,200FFFCC,extended,34029000",
					"patch=1,EE,200FFFD0,extended,FCC20000",
					"patch=1,EE,200FFFD4,extended,03E00008"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ace Combat Zero - The Belkan War (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Aeon Flux (NTSC-U) [CRC: 9FA0A1B0] */
			else if (!strcmp(serial, "SLUS-21205"))
			{
				/* Patch courtesy: Paul Phoenix */
				/* Progressive scan */
				static const char *const patches[] = {
					"patch=1,EE,004481b4,word,3c050000" /* 00052c00 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Aeon Flux (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Ape Escape 2 (NTSC-U) [CRC: BDD9F5E1] */
			else if (!strcmp(serial, "SLUS-20685"))
			{
				/* Patch courtesy: NineKain */
				static const char *const patches[] = {
					"patch=1,EE,00155580,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ape Escape 2 (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Burnout 3: Takedown (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21050"))
			{
				static const char *const patches[] = {
					/* Always ask for 480p mode during boot */
					"patch=0,EE,20437758,extended,100000F1"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: Progressive scan mode (480p) enabled at startup.\n");
			}
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				static const char *const patches[] = {
					/* Always ask for progressive scan */
					"patch=0,EE,2019778C,extended,10A2001C"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Progressive scan mode (480p) enabled at startup.\n");
			}
			/* Capcom Vs. SNK 2 (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20246"))
			{
				static const char *const patches[] = {
					"patch=0,EE,20134DB4,extended,24020002",
					"patch=0,EE,20134DB8,extended,AC22EDD0",
					"patch=0,EE,20134DC8,extended,AC20EDEC"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Capcom Vs. SNK 2 (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Champions of Norrath (NTSC-U) [CRC: 90E66BC5] */
			else if (!strcmp(serial, "SLUS-20565"))
			{
				/* Patch courtesy: Agrippa */
				static const char *const patches[] = {
					/* This game does support two modes -  
					 * 60 fps field mode (1280x448 -> 640x224, half height FB, 
					 * 					      default one)
					 * 30 fps frame mode (1280x448 -> 640x448, full height FB). 
					 *
					 * At the end of each sent frame, the game does reset the 
					 * EE timer, which is used for engine related tasks like 
					 * texture decompressing, physics and, so called, video mode 
					 * switching. Basically, the game does compare the counter 
					 * value with the threshold one and switch the video mode 
					 * accordingly (it is a non-compliant, direct GS register 
					 * write, instead through the 02h syscall). All we have 
					 * to do is to force the full frame mode, leaving the 
					 * 60 ticks timestep intact from the field mode.
					 *
					 * Patching the engineFrameEnd__Fb function
					 * Force the video mode global variables */

					/*trippleBufferMode - used for timestep calculations 
					 * (for 60 tps this value needs to be 0x1) 
					 * and supersampling settings (for frame mode 
					 * it needs to be 0x0) */
					"patch=1,EE,204843E0,extended,00000000",
					/* fullFrameMode - enables the full height front buffer */
					"patch=1,EE,204843E4,extended,00000001",
					/* engineFullFrameMode - enables additional sprites filtering 
					 * - they suck less in the frame mode, it is a engine issue 
					 *   unfortunately (designed for the interlaced and 
					 *   half-pixel offset) */
					"patch=1,EE,20484474,extended,00000001",
					/* Skip the now redundant frameskipping check */
					"patch=1,EE,20190AE4,extended,10000010",
					/* Skip the 30 fps VBlank semaphore, triggered when the 
					 * fullFrameMode is set to 0x1 to delay the frame end */
					"patch=1,EE,20190B48,extended,10000005",
					/* Patching the engineGetAIStepCount__Fv function
					 * Force the 60 ticks timestep (the 0x1 value from 
					 * the trippleBufferMode global). */
					"patch=1,EE,201913D8,extended,24020001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Champions of Norrath (NTSC-U)]: Frame mode (1280x448 -> 640x448) @ 60fps patch applied.\n");
			}
			/* Champions - Return to Arms (NTSC-U) [CRC: 4028A55F] */
			else if (!strcmp(serial, "SLUS-20973"))
			{
				/* Patch courtesy: Agrippa */
				static const char *const patches[] = {
					/* This game does support two modes -  
					 * 60 fps field mode (1280x448 -> 640x224, half height FB,
					 * 					   default one)
					 * 30 fps frame mode (1280x448 -> 640x448, full height FB). 
					 *
					 * At the end of each sent frame, the game does reset the 
					 * EE timer, which is used for engine related tasks like 
					 * texture decompressing, physics and, so called, video 
					 * mode switching. Basically, the game does compare the 
					 * counter value with the threshold one and switch the 
					 * video mode accordingly (it is a non-compliant, 
					 * direct GS register write, instead through the 
					 * 02h syscall). All we have to do is to force the 
					 * full frame mode, leaving the 60 ticks timestep 
					 * intact from the field mode. */
					/* Patching the engineFrameEnd__Fb function
					 * Force the video mode global variables */

					/* trippleBufferMode - used for timestep calculations 
					 * (for 60 tps this value needs to be 0x1) and 
					 * supersampling settings (for frame mode it 
					 * needs to be 0x0) */
					"patch=1,EE,2044FD40,extended,00000000", 

					/* fullFrameMode - enables the full height front buffer */
					"patch=1,EE,2044FD44,extended,00000001", 
					/* engineFullFrameMode - enables additional sprites 
					 * filtering - they suck less in the frame mode, it is 
					 * a engine issue unfortunately (designed for the 
					 * interlaced and half-pixel offset) */
					"patch=1,EE,2044FDB4,extended,00000001",
					/* Skip the now redundant frameskipping check */
					"patch=1,EE,2019DB0C,extended,10000010",
					/* Skip the 30 fps VBlank semaphore, 
					 * triggered when the fullFrameMode 
					 * is set to 0x1 to delay the frame end */
					"patch=1,EE,2019DB6C,extended,10000004",
					/* Patching the engineGetAIStepCount__Fv function
					 * Force the 60 ticks timestep (the 0x1 value from 
					 * the trippleBufferMode global). */
					"patch=1,EE,2019E3AC,extended,24020001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Champions - Return to Arms (NTSC-U)]: Frame mode (1280x448 -> 640x448) @ 60fps patch applied.\n");
			}
			/* Cold Winter (NTSC-U) [CRC: D6D704BB] */
			else if (!strcmp(serial, "SLUS-20845"))
			{
				static const char *const patches[] = {
					"patch=0,EE,10107C60,extended,00000001",
					"patch=0,EE,10679C0C,extended,000001C0",
					"patch=0,EE,1042983C,extended,240201C0",
					"patch=0,EE,10106858,extended,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Cold Winter (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Drakengard (NTSC-U) [CRC: 9679D44C] */
			else if (!strcmp(serial, "SLUS-20732"))
			{
				/* TODO/FIXME - screen cutoff a little on the bottom */
				static const char *const patches[] = {
					/* NOP interlacing */
					"patch=1,EE,204F2668,extended,00000050",
					"patch=1,EE,204F2674,extended,000001E0",
					"patch=1,EE,204F2684,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Drakengard (NTSC-U)]: No-interlacing patch applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Drakengard (NTSC-U)]: TODO/FIXME - Overscan cutoff.\n");
			}
			/* Drakengard 2 (NTSC-U) [CRC: 1648E3C9] */
			else if (!strcmp(serial, "SLUS-21373"))
			{
				/* Patch courtesy: umechan */
				/* TODO/FIXME - screen cutoff a little on the bottom */
				static const char *const patches[] = {
					"patch=1,EE,E0030003,extended,00456DA0",
					"patch=1,EE,20456DA0,extended,00000001",
					"patch=1,EE,20456DB0,extended,00001450",
					"patch=1,EE,20456DBC,extended,001DF9FF",
					"patch=1,EE,E0029400,extended,00456DA0", /* FMV scaling */
					"patch=1,EE,20456DB0,extended,0000948C", /* FMV */
					"patch=1,EE,20456DBC,extended,001DF9FF",
					"patch=1,EE,E0030001,extended,00456D54", /* Game screen */
					"patch=1,EE,20456D38,extended,00000050",
					"patch=1,EE,20456D44,extended,000001E1", /* Game screen scaling */
					"patch=1,EE,20456D54,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Drakengard 2 (NTSC-U)]: No-interlacing patch applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Drakengard 2 (NTSC-U)]: TODO/FIXME - Overscan cutoff.\n");
			}
			/* Enthusia - Professional Racing (NTSC-U) [CRC: 81D233DC] */
			else if (!strcmp(serial, "SLUS-20967")) 
			{
				static const char *const patches[] = {
					"patch=1,EE,2013363C,word,34060001",
					"patch=1,EE,20383A40,word,00009450"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Enthusia - Professional Racing (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Eternal Poison (NTSC-U) [CRC: 2BE55519] */
			else if (!strcmp(serial, "SLUS-21779"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0032DC7C,word,00000000",
					"patch=1,EE,0032DD04,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Eternal Poison (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* God Hand (NTSC-U) [CRC: 6FB69282] */
			else if (!strcmp(serial, "SLUS-21503"))
			{
				static const char *const patches[] = {
					"patch=0,EE,002BE190,extended,24050000",
					"patch=0,EE,002BE194,extended,24060050",
					"patch=0,EE,2030CD10,extended,240E0070",
					"patch=0,EE,2030CD8C,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [God Hand (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Harry Potter and the Sorcerer's Stone (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20826")) 
			{
				/* TODO/FIXME - decouple FPS unlock */
				static const char *const patches[] = {
					"patch=0,EE,2026E528,extended,3405EA60",
					"patch=0,EE,0026E538,extended,24090001",
					"patch=0,EE,1026E914,extended,24030280",
					"patch=0,EE,202E0870,extended,24080001",
					"patch=0,EE,202E1078,extended,0000282D",
					"patch=0,EE,002E08B8,extended,24040002",
					"patch=0,EE,002E00C4,extended,30840002",
					"patch=0,EE,202E077C,extended,24A5FFFF",
					"patch=0,EE,202E1070,extended,24060050",
					"patch=0,EE,102E0854,extended,24030134"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Harry Potter and the Sorcerer's Stone (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* MotoGP 2 (NTSC-U) [CRC: 586EA828] */
			else if (!strcmp(serial, "SLUS-20285"))
			{
				/* Patch courtesy: asasega */
				static const char *const patches[] = {
					"patch=1,EE,20265444,extended,FD030000",
					"patch=1,EE,2027FED0,extended,24020001",
					"patch=1,EE,0043C588,extended,00000001",
					"patch=1,EE,0036C798,extended,00000003",
					"patch=1,EE,0036C7C0,extended,00000003"
				};
				LOAD_PATCHES(patches);

				if (   !strcmp(renderer, "paraLLEl-GS")
				    || !strcmp(renderer, "Software")
				    || !strcmp(renderer, "Software (HW)")
				    || !strcmp(renderer, "Software (SW)"))
				{
					static const char *const patches[] = {
						/* full frame FMV only in software mode */
						"patch=1,EE,0036C798,extended,00000001",
						"patch=1,EE,0036C7C0,extended,00000001",
						"patch=1,EE,2036C7A0,extended,000018D8",
						"patch=1,EE,2036C7C8,extended,000018D8",
					};
					LOAD_PATCHES(patches);
					log_cb(RETRO_LOG_INFO, "[PATCH] [MotoGP 2 (NTSC-U)]: Full-height backbuffer (FRAME) patch for FMVs applied.\n");
				}
				log_cb(RETRO_LOG_INFO, "[PATCH] [MotoGP 2 (NTSC-U)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* MotoGP 3 (NTSC-U) [CRC: 46B7FEC5] */
			else if (!strcmp(serial, "SLUS-20625"))
			{
				/* Patch courtesy: asasega */
				static const char *const patches[] = {
					"patch=1,EE,202C16CC,extended,FD030000",
					"patch=1,EE,202DD564,extended,24020001",
					"patch=1,EE,003EF558,extended,00000003",
					"patch=1,EE,003EF580,extended,00000003"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [MotoGP 3 (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Resident Evil - Code - Veronica X (NTSC-U) [CRC: 24036809] */
			else if (!strcmp(serial, "SLUS-20184"))
			{
				static const char *const patches[] = {
					"patch=0,EE,002CB0A4,extended,24060050",
					"patch=0,EE,202CB0A0,extended,0000282D",
					"patch=0,EE,202CB0B0,extended,00000000",
					"patch=0,EE,201002F4,extended,10A40029",
					"patch=0,EE,1010030C,extended,260202D0",
					"patch=0,EE,00100370,extended,26450023",
					"patch=0,EE,10100398,extended,64E30134",
					"patch=0,EE,102E1AF0,extended,24420134",
					"patch=0,EE,202EB944,extended,00000000",
					"patch=0,EE,202CB0F4,extended,0000482D",
					/* font fixes */
					"patch=1,EE,002B9A50,word,3C013F40",
					"patch=1,EE,002B9A54,word,44816000",
					"patch=1,EE,002B9A58,word,460C6B02",
					"patch=1,EE,002B9A5c,word,3C010050",
					"patch=1,EE,002B9A60,word,E42C8140",
					"patch=1,EE,002B9A64,word,E42D8138",
					"patch=1,EE,002B9A68,word,03E00008",
					"patch=1,EE,002B9A6c,word,E42E8130"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Resident Evil: Code Veronica X (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Resident Evil - Dead Aim (NTSC-U) [CRC: FBB5290C] */
			else if (!strcmp(serial, "SLUS-20669"))
			{
				static const char *const patches[] = {
					"patch=1,EE,2028A268,extended,00000050",
					"patch=1,EE,2028A274,extended,000001E0",
					"patch=1,EE,2028A284,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Resident Evil: Dead Aim (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Rumble Roses (NTSC-U) [CRC: C1C91715] */
			else if (!strcmp(serial, "SLUS-20970"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* Framebuffer Display and no interlacing */
				log_cb(RETRO_LOG_INFO, "[PATCH] [Rumble Roses (NTSC-U)]: Full-height backbuffer (FRAME) patch applied.\n");
				if (!strcmp(renderer, "paraLLEl-GS"))
				{
					static const char *const patches[] = {
						"patch=1,EE,2010291C,extended,00000000",
						"patch=1,EE,20102B84,extended,00000000",
						"patch=1,EE,E0041100,extended,01D4ADA0",
						"patch=1,EE,21D4AD98,extended,00000001",
						"patch=1,EE,21D4ADA0,extended,00001000",
						"patch=1,EE,21D4ADC0,extended,00000001",
						"patch=1,EE,21D4ADC8,extended,00001000"
							/* TODO/FIXME - we're missing the upscaling 
							 * of the menu/startup screens */
					};
					LOAD_PATCHES(patches);
					log_cb(RETRO_LOG_INFO, "[PATCH] [Rumble Roses (NTSC-U)]: TODO/FIXME - menu screens still FIELD.\n");
				}
				else
				{
					static const char *const patches[] = {
						"patch=1,EE,2010291C,extended,00000000",
						"patch=1,EE,20102B84,extended,00000000",
						"patch=1,EE,E0041100,extended,01D4ADA0",
						"patch=1,EE,21D4AD98,extended,00000001",
						"patch=1,EE,21D4ADA0,extended,00001000",
						"patch=1,EE,21D4ADC0,extended,00000001",
						"patch=1,EE,21D4ADC8,extended,00001000"
					};
					LOAD_PATCHES(patches);
				}
			}
			/* Shaun Palmer's Pro Snowboarder (NTSC-U) [CRC: 3A8E10D7] */
			else if (!strcmp(serial, "SLUS-20199"))
			{
				/* Patch courtesy: felixthechat1970 */
				static const char *const patches[] = {
					/* test s.backbuffer - frame mode by felixthecat1970 */
					/* menu is field render, use deinterlacing=auto */
					"patch=0,EE,2012B6C4,extended,0000102D",
					"patch=0,EE,2012B6E8,extended,00041803",
					"patch=0,EE,2012B714,extended,0000502D",
					"patch=0,EE,2012B730,extended,0000282D",
					"patch=0,EE,2012B750,extended,00083003",
					"patch=0,EE,2012B780,extended,0000502D"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Shaun Palmer's Pro Snowboarder (NTSC-U)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* Tales of Legendia (NTSC-U) [CRC: 43AB7214] */
			else if (!strcmp(serial, "SLUS-21201"))
			{
				static const char *const patches[] = {
					"patch=1,EE,D03F9750,extended,00001000",
					"patch=1,EE,103F9750,extended,000010E0"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tales of Legendia (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Tekken Tag Tournament (NTSC-U) [CRC: 67454C1E] */
			else if (!strcmp(serial, "SLUS-20001")) 
			{
				static const char *const patches[] = {
					"patch=0,EE,20398960,extended,0000382D",
					"patch=0,EE,20398AF0,extended,0000502D",
					"patch=0,EE,10398AE0,extended,240701C0",
					"patch=0,EE,20398AF0,extended,0000502D",
					"patch=0,EE,10398B10,extended,240701C0",
					"patch=0,EE,10398B38,extended,240701C0",
					"patch=0,EE,20398B48,extended,0000502D"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken Tag Tournament (NTSC-U)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* Tekken 5 (NTSC-U) [CRC: 652050D2] */
			else if (!strcmp(serial, "SLUS-21059")) 
			{
				/* Patch courtesy: felixthecat1970 */
				/* TODO/FIXME - decouple widescreen */
				static const char *const patches[] = {
					"patch=0,EE,00D05EC8,extended,24050000",
					"patch=0,EE,00D05ECC,extended,24060050",
					"patch=0,EE,20D05ED4,extended,24070001",
					/* Devil Within upscaling */
					"patch=1,EE,E0078870,extended,01FFEF20",
					"patch=1,EE,202DE308,extended,AC940004", /* enable progressive at start - skips Starblade minigame */
					"patch=1,EE,202F06DC,extended,341B0001",
					"patch=1,EE,202F08FC,extended,A07B0000",
					/* sharp backbuffer main game - skips StarBlade intro game */
					"patch=1,EE,0031DA9C,extended,30630000",
					"patch=1,EE,00335A38,extended,24020001",
					"patch=1,EE,20335A5C,extended,00031C02",
					"patch=1,EE,20335E58,extended,00042402",
					/* Devil Within - sharp backbuffer */
					"patch=1,EE,E0020001,extended,0027E448",
					"patch=1,EE,2027E448,extended,00500000",
					"patch=1,EE,203F7330,extended,00500000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken 5 (NTSC-U)]: Progressive scan mode (480p) enabled at startup.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken 5 (NTSC-U)]: skips StarBlade intro game.\n");
			}
			/* Urban Reign (NTSC-U) [CRC: BDD9BAAD] */
			else if (!strcmp(serial, "SLUS-21209"))
			{
				static const char *const patches[] = {
					"patch=1,EE,201372e0,extended,0C04DCEC",
					"patch=1,EE,201372e8,extended,0C04DCEC"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Urban Reign (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Virtua Fighter 4 - Evolution (NTSC-U) [CRC: C9DEF513] */
			else if (!strcmp(serial, "SLUS-20616")) 
			{
				static const char *const patches[] = {
					"patch=1,EE,E0054470,extended,01FFFF20",

					"patch=1,EE,203A798C,extended,0000182D",
					"patch=1,EE,00532120,extended,00000050",
					"patch=1,EE,203A7624,extended,00A32825",
					"patch=1,EE,203A76C8,extended,00A32825",
					"patch=1,EE,103A8220,extended,2484013A", /* 2484013A */
					"patch=1,EE,E0050E70,extended,01FFFF20", /* Virtua Fighter: 10th Anniversary Edition */
					"patch=1,EE,203A509C,extended,0000182D",
					"patch=1,EE,0052F1D0,extended,00000050",
					"patch=1,EE,203A4D34,extended,00A32825",
					"patch=1,EE,203A4DD8,extended,00A32825",
					"patch=1,EE,103A5930,extended,2484013A"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Virtua Fighter 4: Evolution (NTSC-U)]: No-interlacing patch applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Virtua Fighter 4: Evolution (NTSC-U)]: TODO/FIXME - positioning is off.\n");
			}
			/* Whiplash (NTSC-U) [CRC: 4D22DB95] */
			else if (!strcmp(serial, "SLUS-20684"))
			{
				static const char *const patches[] = {
					"patch=1,EE,2025DFA4,extended,30630000",
					"patch=1,EE,20353958,extended,34030001",
					"patch=1,EE,2035396C,extended,34029040"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Whiplash (NTSC-U)]: No-interlacing patch applied.\n");
			}
		}
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* God of War II (NTSC-U) [CRC: 2F123FD8] */
			if (!strcmp(serial, "SCUS-97481"))
			{
				static const char *const patches[] = {
					/* Default to progressive scan at first run */
					"patch=1,EE,0025a608,word,a04986dc",
					"patch=1,EE,001E45D4,word,24020001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [God of War II (NTSC-U)]: Progressive scan mode (480p) enabled at startup.\n");
			}
			/* Gran Turismo 4: Mazda MX-5 Edition (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SCUS-97483"))
			{
				/* Patch courtesy: Blackbird+Silent */
				static const char *const patches[] = {
					/* Autoboot in 480p */
					"patch=1,EE,20A1C070,extended,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gran Turismo 4: Mazda MX-5 Edition (NTSC-U)]: Progressive scan mode (480p) enabled at startup.\n");
			}
			/* Gran Turismo 4 (NTSC-U) [CRC: 77E61C8A] */
			else if (!strcmp(serial, "SCUS-97328"))
			{
				static const char *const patches[] = {
					/* Autoboot mode NTSC=0 / 480p=1 / 1080i=2 
					 * (change last number) or disable this code. */
					"patch=1,EE,20A461F0,extended,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gran Turismo 4 (NTSC-U)]: Progressive scan mode (480p) enabled at startup.\n");
			}
			/* ICO (NTSC-U) [CRC: 6F8545DB] */
			else if (!strcmp(serial, "SCUS-97113")) 
			{
				static const char *const patches[] = {
					/* enable back buffer */
					"patch=0,EE,00274EF8,extended,00000001",
					"patch=0,EE,00274F20,extended,00000001",
					"patch=0,EE,00274F00,extended,00001040",
					"patch=0,EE,00274F28,extended,00001040",
					/* nointerlacing */
					"patch=1,EE,00274EF8,extended,00000001",
					"patch=1,EE,00274F20,extended,00000001",
					"patch=1,EE,00274F00,extended,00000040",
					"patch=1,EE,00274F28,extended,00000040"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [ICO (NTSC-U)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* Kinetica (NTSC-U) [CRC: D39C08F5] */
			else if (!strcmp(serial, "SCUS-97132"))
			{
				/* Patch courtesy: Mensa */
				/* Stops company logos and intro FMV from shaking. 
				 * Menus and in-game never had an issue */
				static const char *const patches[] = {
					"patch=1,EE,201ABB34,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Kinetica (NTSC-U)]: No-interlacing patch applied.\n");
			}
			/* Tourist Trophy (NTSC-U)  */
			else if (!strcmp(serial, "SCUS-97502"))
			{
				/* Patch courtesy: Blackbird+Silent */
				static const char *const patches[] = {
					"patch=1,EE,20829248,extended,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tourist Trophy (NTSC-U)]: Progressive scan mode (480p) enabled at startup.\n");
			}
		}
		else if (!strncmp("SCES-", serial, strlen("SCES-")))
		{
			/* Ace Combat: Squadron Leader (PAL) [CRC: 1D54FEA9] */
			if (!strcmp(serial, "SCES-52424"))
			{
				static const char *const patches[] = {
					/* NOP the addition of front buffer address */
					"patch=1,EE,0032B0A8,word,00000000", /* 00A22825 */
					/* set the SMODE2 register to FRAME mode */
					"patch=1,EE,003311B8,word,00000000", /* 14400002 */
					/* force the 448 height for GS_DISPLAY2 
					 * register calculations (back buffer height is 448) */
					"patch=1,EE,00331124,word,241200E0", /* 00079403 */
					/* Last minute lazy fix for stuttering FMVs. Game does 
					 * render the prerecorded movies into the two interleaved 
					 * buffers. We need to remove the first patch when the 
					 * FMVs are played. */
					"patch=1,EE,E0011400,extended,0059660C",
					"patch=1,EE,2032B0A8,extended,00A22825"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ace Combat: Squadron Leader (PAL)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* Gran Turismo 4 (PAL) [CRC: 44A61C8F] */
			else if (!strcmp(serial, "SCES-51719"))
			{
				/* Patch courtesy: agrippa */
				static const char *const patches[] = {
					/* progressive flag (write 0x1 at 0xA57E70). */
					"patch=1,EE,000FF000,word,24020001",
					"patch=1,EE,000FF004,word,3C0300A5",
					"patch=1,EE,000FF008,word,34637E70",
					"patch=1,EE,000FF00c,word,AC620000",
					"patch=1,EE,000FF010,word,08128308",
					"patch=1,EE,000FF014,word,000422C2",
					"patch=1,EE,004A0C18,word,0803FC00", /* 000422C2 */
					/* force SCE_GS_PAL video mode every time 
					 * (when the progressive flag is set, 
					 * the 480p mode is turned on instead by default) */
					"patch=1,EE,001074A0,word,24050003", /* 8E050004 */
					/* change sceGsResetGraph arguments when the 
					 * progressive mode is set. These patches are 
					 * essential when playing on a real hardware 
					 * (half of the screen is only visible, otherwise): */
					/* SCE_GS_INTERLACE */
					"patch=1,EE,0061868C,word,00000001", /* 00000000 */
					/* SCE_GS_FIELD */
					"patch=1,EE,00618694,word,00000000", /* 00000001 */
					/* no interlacing patch for menu and movies 
					 * (delete this if you play on a real hardware) */
					"patch=1,EE,004A2A2C,word,0000102D" /* 80A202C0 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gran Turismo 4 (PAL)]: Progressive scan mode (480p) enabled at startup.\n");
			}
			/* ICO (PAL) [CRC: 5C991F4E] */
			else if (!strcmp(serial, "SCES-50760"))
			{
				/* Patch courtesy: agrippa */
				static const char *const patches[] = {
					/* Set the back buffer */
					"patch=1,EE,2028F500,extended,00001040",
					"patch=1,EE,2028F528,extended,00001040",
					/* Switch to the interlaced mode with FFMD set to 0. 
					 * Progressive mode, applied by default,
					 * does add a black bar at the bottom in the NTSC mode 
					 * when the back buffer is enabled */
					"patch=1,EE,2028F4F8,extended,00000001",
					"patch=1,EE,2028F520,extended,00000001",
					/* Check if the PAL mode is turned on to extend 
					 * the display buffer from 256 to 512 */
					"patch=1,EE,E0024290,extended,0028F508",
					"patch=1,EE,2028F50C,extended,001FF9FF",
					"patch=1,EE,2028F534,extended,001FF9FF",
					/* Check if the NTSC mode is turned on to extend 
					 * the display buffer from 224 to 448 */
					"patch=1,EE,E002927C,extended,0028F508",
					"patch=1,EE,2028F50C,extended,001DF9FF",
					"patch=1,EE,2028F534,extended,001DF9FF"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [ICO (PAL)]: Full-height backbuffer (FRAME) applied.\n");
			}
			/* Soulcalibur III (PAL) v1.00 [CRC: BC5480A3] */
			else if (!strcmp(serial, "SCES-53312")) 
			{
				/* Patch courtesy: Agrippa */
				static const char *const patches[] = {
					/* Rename PAL60/60 Hz with Progressive */
					"patch=1,EE,00B73FB4,word,6F725020",
					"patch=1,EE,00B73FB8,word,73657267",
					"patch=1,EE,00B73FBC,word,65766973",
					"patch=1,EE,00B73FC0,word,0A205D20"
				};
				/* Replace NTSC mode with Progressive Scan */
				if (     game_crc == 0xBC5480A3) /* 1.00 */
					LoadPatchesFromString("patch=1,EE,00509E4A,byte,06");
				else if (game_crc == 0x3BA95B70) /* 2.00 */
					LoadPatchesFromString("patch=1,EE,00509ECA,byte,06");

				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Soulcalibur III (PAL)]: Progressive scan mode (480p) enabled at startup.\n");
			}
			/* Tekken Tag Tournament (PAL) [CRC: 0DD8941C] */
			else if (!strcmp(serial, "SCES-50001")) 
			{
				static const char *const patches[] = {
					"patch=0,EE,203993D0,extended,0000382D",
					"patch=0,EE,10399580,extended,240700E0",
					"patch=0,EE,103995A8,extended,240701C0",
					"patch=0,EE,203995B8,extended,0000502D",
					"patch=0,EE,2039DDE8,extended,0000382D"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken Tag Tournament (PAL)]: Full-height backbuffer patch applied.\n");
			}
			/* Tekken 4 (PAL) */
			else if (!strcmp(serial, "SCES-50878"))
			{
				/* Patch courtesy: felixthecat1970 */
				static const char *const patches[] = {
					"patch=0,EE,001E2254,extended,24020002",
					"patch=0,EE,0022B138,extended,24050006",
					"patch=0,EE,001EDC24,extended,24020009"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken 4 (PAL)]: Progressive scan mode (480p) enabled at startup.\n");
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* Colin McRae Rally 3 (PAL) [CRC: 7DEAE69C] */
			if (!strcmp(serial, "SLES-51117")) 
			{
				/* Patch courtesy: agrippa */
				static const char *const patches[] = {
					"patch=1,EE,00246B90,word,24040001", 
					/* set FFMD to 0 in SMODE2 register to 
					 * disable field mode */
					"patch=1,EE,00247A64,word,00000000"  
						/* NOP the switch to the front buffer 
						 * A full height back buffer enabled, 
						 * instead of a downsampled front buffer. */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Colin McRae Rally 3 (PAL)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* Resident Evil - Dead Aim (PAL) [CRC: F79AF536] */
			else if (!strcmp(serial, "SLES-51448")) 
			{
				/* Patch courtesy: dante3732 */
				static const char *const patches[] = {
					"patch=1,EE,2028AB88,extended,00000050",
					"patch=1,EE,2028AB94,extended,000001E0",
					"patch=1,EE,2028ABA4,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Resident Evil: Dead Aim (PAL)]: No-interlacing patch applied.\n");
			}
			/* Star Ocean: Til the End of Time (PAL) [CRC: E04EA200] */
			else if (!strcmp(serial, "SLES-82028"))
			{
				/* Patch courtesy: agrippa */
				static const char *const patches[] = {
					/* Skip the integrity check of the main executable file */
					"patch=1,EE,E0110011,extended,001F7660",
					"patch=1,EE,201e2530,extended,10000016",
					"patch=1,EE,201e2ff8,extended,10000016",
					"patch=1,EE,201e3410,extended,10000016",
					"patch=1,EE,201e3758,extended,10000016",
					"patch=1,EE,201e3968,extended,10000016",
					"patch=1,EE,201e3ba8,extended,10000016",
					"patch=1,EE,201e3d00,extended,10000016",
					"patch=1,EE,201eb5f8,extended,10000016",
					"patch=1,EE,201f68c0,extended,10000016",
					"patch=1,EE,201f6bb0,extended,10000016",
					"patch=1,EE,201f6c50,extended,10000016",
					"patch=1,EE,201f7030,extended,10000016",
					"patch=1,EE,201f7160,extended,10000016",
					"patch=1,EE,201f72a0,extended,10000016",
					"patch=1,EE,201f73d0,extended,10000016",
					"patch=1,EE,201f7500,extended,10000016",
					"patch=1,EE,201f7660,extended,10000016",
					/* in-battle anti-cheat checks? I have 
					 * not seen the game to get there though. */
					"patch=1,EE,E002FFFA,extended,001EDB44",
					"patch=1,EE,201EDB44,extended,1400fffa",
					"patch=1,EE,201E94E0,extended,1000000F", /* 1440000F */
					/* full height frame buffer and video mode patches */
					"patch=0,EE,00101320,word,A0285C84", /* A0205C84 */
					"patch=1,EE,0012EF60,word,00000000", /* 10C00005 */
					"patch=1,EE,00100634,word,24050001", /* 0000282D */
					"patch=1,EE,00100638,word,24060003", /* 24060050 */
					"patch=1,EE,00100640,word,24070000", /* 24070001 */
					/* Texture fix for the battle mode */
					"patch=1,EE,E0011183,extended,001E0784",
					"patch=1,EE,201E0784,extended,24021D00"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Star Ocean: Til the End of Time (PAL)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* Valkyrie Profile 2: Silmeria (PAL) [CRC: 04CCB600] */
			else if (!strcmp(serial, "SLES-54644"))
			{
				/* Patch courtesy: agrippa */
				static const char *const patches[] = {
					/* force progressive mode flag */
					"patch=1,EE,00101100,word,A025CC84", /* A020CC84 */
					/* force PAL output when the progressive mode is set */
					"patch=1,EE,00100624,word,24050001", /* 0000282D */
					"patch=1,EE,00100628,word,24060003", /* 24060050 */
					"patch=1,EE,00100630,word,24070000", /* 24070001 */
					/* correct the screen position on y-axis 
					 * (important when playing on a real hardware!) */
					"patch=1,EE,00117178,word,02A01021", /* 26A20014 */
					/* skip overwriting the DISPLAY2 register values 
					 * with the 480p ones */
					"patch=1,EE,00117388,word,10000011", /* 10600011 */
					/* fix the missing icons in battle mode */
					"patch=1,EE,E0011983,extended,00361AE4",
					"patch=1,EE,20361AE4,extended,24031000",
					/* disable the photon warp effect to avoid freeze */
					"patch=1,EE,E0010012,extended,00374BD4",
					"patch=1,EE,20374BD4,extended,10000012",
					/* The first frame of the transition screen is glitched, 
					 * as the garbage is being written into the frame buffer. */
					"patch=1,EE,003078EC,word,24030200", /* 24030080 */
					/* GameGuard disable codes in the PNACH format, 
					 * ported from the Maori-Jigglypuff's original overlay codes */
					"patch=1,EE,00100A7C,word,1400FFFA",
					/* Disable the memory scanning in the 2D mode */
					"patch=1,EE,E0038C56,extended,00423722",
					"patch=1,EE,204233B4,extended,24160000", /* 8C560000 */
					"patch=1,EE,20423568,extended,24160000", /* 8C560000 */
					"patch=1,EE,20423720,extended,24160000", /* 8C560000 */
					/* Skip the traps while saving */
					"patch=1,EE,E0016EA4,extended,00499F5C",
					"patch=1,EE,20499F5C,extended,00000000", /* 0C126EA4 */
					/* Disable the protection traps and 
					 * memory scanning in the battle mode */
					"patch=1,EE,E00D002D,extended,00431950",
					"patch=1,EE,20397BB0,extended,00000000", /* 1440FFF9 */
					"patch=1,EE,203ABE70,extended,10000017", /* 1C600017 */
					"patch=1,EE,203AF11C,extended,00000000", /* 1000FFFA */
					"patch=1,EE,203AF2B4,extended,00000000", /* 1000FFFA */
					"patch=1,EE,203AF374,extended,00000000", /* 1000FFFA */
					"patch=1,EE,203C5484,extended,00000000", /* 1000FFFA */
					"patch=1,EE,203CFA7C,extended,00000000", /* 1000FFFA */
					"patch=1,EE,203D226C,extended,10000020", /* 14200020 */
					"patch=1,EE,203D4554,extended,00000000", /* 1000FFFA */
					"patch=1,EE,203ACFB4,extended,100000F4", /* 1020002E */
					"patch=1,EE,203AD464,extended,1000003C", /* 1020002E */
					"patch=1,EE,20431798,extended,10000055", /* 1000002D */
					"patch=1,EE,20431950,extended,10000043", /* 1000002D */
					/* Pre and post battle integrity check */
					"patch=1,EE,E0030038,extended,0036829C",
					"patch=1,EE,20367F38,extended,100000D5", /* 102000D5 */
					"patch=1,EE,20368294,extended,00000000"  /* FD690008 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Valkyrie Profile 2: Silmeria (PAL)]: Progressive scan mode (480p) enabled at startup.\n");
			}
			/* Virtua Fighter 4: Evolution (PAL) [CRC: 81CA29BE] */
			else if (!strcmp(serial, "SLES-51616"))
			{
				/* Patch courtesy: Agrippa */
				static const char *const patches[] = {
					/* SCE_GS_FIELD set to 0x0 - read the full frame 
					 * instead of the half */
					"patch=1,EE,002FAF74,word,24070000", /* 00C0382D */
					/* replace front buffer addresses with back buffer ones */
					"patch=1,EE,002FABA4,word,00A32825", /* 00A62825 */
					"patch=1,EE,002FAC48,word,00A32825", /* 00A62825 */
					/* force the 448 frame height */
					"patch=1,EE,002FAB14,word,3C01001B", /* 3C01001F */
					/* disable the scaling of frame buffer in the PAL mode */
					"patch=1,EE,002F74A4,word,10000006" /* 14620006 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Virtua Fighter 4 Evolution (PAL)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
		}
		else if (!strncmp("SLPS-", serial, strlen("SLPS-")))
		{
			/* Alpine Racer 3 (NTSC-J) [CRC: 771C3B47] */
			if (!strcmp(serial, "SLPS-20181"))
			{
				/* Patch courtesy: asasega */
				static const char *const patches[] = {
					"patch=1,EE,E00410E0,extended,00686C80",
					"patch=1,EE,20686C78,extended,00000001",
					"patch=1,EE,20686C80,extended,00001000",
					"patch=1,EE,20686CA0,extended,00000001",
					"patch=1,EE,20686CA8,extended,00001000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Alpine Racer 3 (NTSC-J)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Capcom Vs. SNK 2 (NTSC-J) [CRC: ] */
			if (!strcmp(serial, "SLPM-74246"))
			{
				static const char *const patches[] = {
					"patch=0,EE,20134E44,extended,24020002",
					"patch=0,EE,20134E48,extended,AC222990",
					"patch=0,EE,20134E58,extended,AC2029AC"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Capcom Vs. SNK 2 (NTSC-J)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* Mushihimesama (NTSC-J) [CRC: F0C24BB1] */
			else if (!strcmp(serial, "SLPM-66056"))
			{
				/* Patch courtesy: asasega */
				static const char *const patches[] = {
					"patch=1,EE,2010C300,extended,34030001",
					"patch=1,EE,2010C314,extended,3402148C",
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Mushihimesama (NTSC-J)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* Rumble Fish, The (NTSC-J) */
			else if (!strcmp(serial, "SLPM-65919"))
			{
				/* Patch courtesy: felixthecat1970 */
				static const char *const patches[] = {
					/* Framebuffer + 480p mode + No interlacing */
					"patch=0,EE,201102A4,extended,3C050000",
					"patch=0,EE,201102AC,extended,3C060050",
					"patch=0,EE,201102B4,extended,3C070001",
					"patch=0,EE,20110948,extended,34030002",
					"patch=1,EE,2034FD50,extended,00009446",
					"patch=1,EE,2034FD5C,extended,001DF4FF",
					"patch=1,EE,2034FD78,extended,00009446",
					"patch=1,EE,2034FD84,extended,001DF4FF",
					/* NULL Int ints */
					"patch=0,EE,20111278,extended,03E00008",
					"patch=0,EE,2011127C,extended,00000000",
					"patch=0,EE,201114E0,extended,03E00008",
					"patch=0,EE,201114E4,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Rumble Fish, The (NTSC-J)]: Full-height backbuffer (FRAME) patch applied.\n");
			}
			/* Sega Rally 2006 (NTSC-J) [CRC: B26172F0] */
			else if (!strcmp(serial, "SLPM-66212"))
			{
				/* Patch courtesy: asasega */
				static const char *const patches[] = {
					"patch=1,EE,20106FA0,extended,34030001",
					"patch=1,EE,20106FB4,extended,34021040"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sega Rally 2006 (NTSC-J)]: No-interlacing patch applied.\n");
			}
			/* Virtua Fighter 10th Anniversary (NTSC-J) [CRC: B5FEAE85] */
			else if (!strcmp(serial, "SLPM-68008"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* 640x224 to 640x448
				 * 512x224 to 512x448 */
				static const char *const patches[] = {
					"patch=1,EE,203A18B8,extended,0000102D",
					"patch=1,EE,203A5644,extended,0000382D",
					"patch=1,EE,203A5274,extended,00A32825",
					"patch=1,EE,203A5318,extended,00A32825"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Virtua Fighter 10th Anniversary (NTSC-J)]: No-interlacing patch applied.\n");
			}
		}
	}

	if (hint_disable_mipmaps)
	{
		/* The games listed below need patches when mipmapping
		 * is set to unclamped */

		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* Ace Combat 5 - The Unsung War (NTSC-U) [CRC: 39B574F0] */
			if (!strcmp(serial, "SLUS-20851"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0011F2CC,word,00000000",
					"patch=1,EE,0011F2DC,word,00000000",
					"patch=1,EE,0011F2E8,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ace Combat 5 - The Unsung War (PAL)]: Mipmap disable patch applied.\n");
			}
			/* Aggressive Inline (NTSC-U) [CRC: 67DB3ED8] */
			else if (!strcmp(serial, "SLUS-20327"))
			{
				static const char *const patches[] = {
					"patch=1,EE,001090B0,word,45010009"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Aggressive Inline (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Ape Escape 2 (NTSC-U) [CRC: BDD9F5E1] */
			else if (!strcmp(serial, "SLUS-20685"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0034CE88,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ape Escape 2 (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* BMX XXX (SLUS-20415) [CRC: 2999BCF9] */
			else if (!strcmp(serial, "SLUS-20415"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00108610,word,10000009"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [BMX XXX (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* F1 Career Challenge (SLUS-20693) [CRC: 2C1173B0] */
			else if (!strcmp(serial, "SLUS-20693"))
			{
				/* Patch courtesy: agrippa */
				static const char *const patches[] = {
					"patch=1,EE,00257a40,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [F1 Career Challenge (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* FIFA 2003 (SLUS-20580) [CRC: 67C38BAA] */
			else if (!strcmp(serial, "SLUS-20580"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0030F5FC,word,10000079"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [FIFA 2003 (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Harry Potter - Quidditch World Cup (NTSC-U) [CRC: 39E7ECF4] */
			else if (!strcmp(serial, "SLUS-20769"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002ABD7C,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Harry Potter - Quidditch World Cup (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Harry Potter and the Goblet of Fire (NTSC-U) [CRC: B38CC628] */
			else if (!strcmp(serial, "SLUS-21325"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002CF158,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Harry Potter and the Goblet of Fire (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Lara Croft Tomb Raider - Anniversary (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-21555"))
			{
				static const char *const patches[] = {
					"patch=1,EE,001297C0,word,10000022"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Lara Croft Tomb Raider - Anniversary (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Lara Croft Tomb Raider - Legend (NTSC-U) [CRC: BC8B3F50] */
			else if (!strcmp(serial, "SLUS-21203"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00127390,word,10000022"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Lara Croft Tomb Raider - Legend (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Legacy of Kain: Defiance (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20773"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00188F50,word,10000020"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Legacy of Kain: Defiance (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Legacy of Kain: Soul Reaver 2, The (NTSC-U) [CRC: 1771BFE4] */
			else if (!strcmp(serial, "SLUS-20165"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0029FC00,word,000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Legacy of Kain: Soul Reaver 2, The (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Quake III - Revolution (NTSC-U) [CRC: A56A0525] */
			else if (!strcmp(serial, "SLUS-20167"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002D0398,word,03E00008"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Quake III: Revolution (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Whiplash (NTSC-U) [CRC: 4D22DB95] */
			else if (!strcmp(serial, "SLUS-20684"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0025D19C,word,10000007"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Whiplash (NTSC-U)]: Mipmap disable patch applied.\n");
			}
		}
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* Jak II: Renegade (NTSC-U) [CRC: 9184AAF1] */
			if (!strcmp(serial, "SCUS-97265"))
			{
				static const char *const patches[] = {
					"patch=1,EE,005F8D08,word,10000016"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Jak II: Renegade (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Jak III (NTSC-U) [CRC: 644CFD03] */
			else if (!strcmp(serial, "SCUS-97330"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0059F570,word,10000016"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Jak III (NTSC-U)]: Mipmap disable patch applied.\n");
			}
			/* Jak X [CRC: 3091E6FB] */
			else if (!strcmp(serial, "SCUS-97574"))
			{
				static const char *const patches[] = {
					"patch=1,EE,007AEB70,word,10000016",
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Jak X (NTSC-U)]: Mipmap disable patch applied.\n");
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* Aggressive Inline (PAL-M) [CRC: D6A0D7A5] */
			if (!strcmp(serial, "SLES-50480"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00109130,word,45010009"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Aggressive Inline (PAL)]: Mipmap disable patch applied.\n");
			}
			/* BMX XXX (SLES-51365) [CRC: 3A48B51C] */
			else if (!strcmp(serial, "SLES-51365"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00108780,word,10000009"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [BMX XXX (PAL)]: Mipmap disable patch applied.\n");
			}
			/* F1 Career Challenge (SLES-51584) [CRC: 2C1173B0] */
			else if (!strcmp(serial, "SLES-51584"))
			{
				/* Patch courtesy: agrippa */
				static const char *const patches[] = {
					"patch=1,EE,00257a40,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [F1 Career Challenge (PAL)]: Mipmap disable patch applied.\n");
			}
			/* FIFA 2003 (SLES-51197) [CRC: 722BBD62] */
			else if (!strcmp(serial, "SLES-51197"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0030F554,word,10000079"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [FIFA 2003 (PAL)]: Mipmap disable patch applied.\n");
			}
			/* Harry Potter - Quidditch World Cup (PAL) */
			else if (!strcmp(serial, "SLES-51787"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002ABD4C,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Harry Potter: Quidditch World Cup (PAL)]: Mipmap disable patch applied.\n");
			}
			/* Harry Potter and the Goblet of Fire (PAL) [CRC: B38CC628] */
			else if (    
					!strcmp(serial, "SLES-53728")
					|| !strcmp(serial, "SLES-53727")
					|| !strcmp(serial, "SLES-53726")
				)
			{
				static const char *const patches[] = {
					"patch=1,EE,002CF158,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Harry Potter and the Goblet of Fire (PAL)]: Mipmap disable patch applied.\n");
			}
			/* Legacy of Kain: Soul Reaver 2, The (NTSC-U) [CRC: 1771BFE4] */
			else if (!strcmp(serial, "SLES-50197"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002A1F80,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Legacy of Kain: Soul Reaver 2, The (PAL)]: Mipmap disable patch applied.\n");
			}
			/* Quake III - Revolution (PAL) [CRC: ] */
			else if (!strcmp(serial, "SLES-50126"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002D0320,word,27BDFF40"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Quake III: Revolution (PAL)]: Mipmap disable patch applied.\n");
			}
			/* Quake III - Revolution (PAL) [CRC: ] */
			else if (!strcmp(serial, "SLES-50127"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002D0328,word,27BDFF40"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Quake III: Revolution (PAL)]: Mipmap disable patch applied.\n");
			}
		}
		else if (!strncmp("SCES-", serial, strlen("SCES-")))
		{
			/* Ape Escape 2 (PAL) [CRC: 09B3AD4D] */
			if (!strcmp(serial, "SCES-50885"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0034E0E0,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ape Escape 2 (PAL)]: Mipmap disable patch applied.\n");
			}
		}
		else if (!strncmp("SLPS-", serial, strlen("SLPS-")))
		{
			/* F1 Career Challenge (NTSC-J) [CRC: 5CBB11E6] */
			if (!strcmp(serial, "SLPS-20295"))
			{
				/* Patch courtesy: agrippa */
				static const char *const patches[] = {
					"patch=1,EE,002581d8,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [F1 Career Challenge (NTSC-J)]: Mipmap disable patch applied.\n");
			}
			/* FIFA 2003 (SLPS-25179) [CRC: A6A8DAB8] */
			else if (!strcmp(serial, "SLPS-25179"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0030F5EC,word,10000079"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [FIFA 2003 (NTSC-J)]: Mipmap disable patch applied.\n");
			}
			/* Quake III - Revolution (NTSC-J) [CRC: ] */
			else if (!strcmp(serial, "SLPS-20108"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002D2F70,word,27BDFF40"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Quake III: Revolution (NTSC-J)]: Mipmap disable patch applied.\n");
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Harry Potter - Quidditch World Cup (NTSC-J) */
			if (!strcmp(serial, "SLPM-62408"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002ABC04,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Harry Potter - Quidditch World Cup (NTSC-J)]: Mipmap disable patch applied.\n");
			}
		}
	}

	if (fastcdvd)
	{
		/* Shadow Man: 2econd Coming (NTSC-U) [CRC: 60AD8FA7] */
		if (!strcmp(serial, "SLUS-20413"))
		{
			/* Only works with fastcdvd when enabling these patches */
			static const char *const patches[] = {
				"patch=1,IOP,000884e8,word,34048800",
				"patch=1,IOP,000884ec,word,34048800",
				"patch=1,IOP,00088500,word,34048800",
				"patch=1,IOP,0008850c,word,34048800",
				"patch=1,IOP,000555e8,word,34048800",
				"patch=1,IOP,000555ec,word,34048800",
				"patch=1,IOP,00055600,word,34048800",
				"patch=1,IOP,0005560c,word,34048800"
			};
			LOAD_PATCHES(patches);
			log_cb(RETRO_LOG_INFO, "[PATCH] [Shadow Man: 2econd Coming (NTSC-U)]: Compatibility patch for FastCDVD applied.\n");
		}
	}

	if (hint_game_enhancements)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* Alias (NTSC-U) [CRC: E3ADDC73] */
			if (!strcmp(serial, "SLUS-20673")) /* what is this ? */
			{
				static const char *const patches[] = {
					"patch=1,EE,2023C104,word,24030001",
					"patch=1,EE,2023C108,word,AC431E5C"
				};
				LOAD_PATCHES(patches);
			}
			/* Bloody Roar 3 (NTSC-U) [CRC: AA4E5A35] */
			else if (!strcmp(serial, "SLUS-20212"))
			{
				static const char *const patches[] = {
					/* Restore the blood effects intensity 
					 * (just like the Japanese version) */
					"patch=1,EE,0012d638,word,24040080",
					"patch=1,EE,001bb2cc,word,24020080"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Bloody Roar 3 (NTSC-U)]: Restore Japanese blood effects intensity enhancement patch applied.\n");
			}
			/* Burnout 3: Takedown (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21050"))
			{
				static const char *const patches[] = {
					/* Enable props in Road Rage mode */
					"patch=0,EE,201B9F60,extended,00000000",
					"patch=0,EE,202F9A44,extended,00000000",
					/* Unlimited explosions (also affects crash mode) */
					"patch=0,EE,201BBA08,extended,00000000",
					/* Render extra particles while driving */
					"patch=0,EE,20261EB8,extended,24040001",
					/* Use 255 colors in garage. 
					 * (Doesn't jump to 254 after the 8th color.) */
					"patch=1,EE,2042BCE8,extended,70A028E8",
					/* bypass PVS/force render all immediate units */
					"patch=1,EE,20301EAC,extended,00000000",
					/* Force specific LOD */
					"patch=0,EE,00151ABF,extended,00000010",
					/* Last digit is LOD level, 
					 * 0, 1, 2, 3, and 4 (4 being the most detailed iirc) */
					"patch=0,EE,20151B78,extended,24070004",
					"patch=0,EE,20261E6C,extended,24120001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: Enhancement patches applied:\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: Enable props in Road Rage mode.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: Unlimited explosions (also affects crash mode).\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: Render extra particles while driving.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: Use 255 colors in garage (doesn't jump to 254 after the 8th color).\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: Bypass PVS/force render all immediate units.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: Force max LOD cars.\n");
			}
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				static const char *const patches[] = {
					/* Enable props in World Tour Road Rage */
					"patch=0,EE,20129FF8,extended,00000000",
					/* Enable props in Multiplayer/Single Event Road Rage */
					"patch=0,EE,2012648C,extended,00000000",
					/* Enable props in Traffic Attack mode */
					"patch=0,EE,20123C1C,extended,00000000",
					/* Force race cars LOD to 5 */
					"patch=0,EE,202D1660,extended,03E00008",
					"patch=0,EE,202D1664,extended,24020004",
					/* Prevent race cars reflections from fading further away */
					"patch=0,EE,202D165C,extended,E4C30000",
					/* Falling car parts while driving 
					 * (takedowns and traffic checks) */
					"patch=0,EE,20210FA8,extended,00000000",
					/* Prevent the game to remove out of range crashing traffic */
					"patch=0,EE,001EC88A,extended,00000000",
					/* Render all extra particles while driving */
					"patch=0,EE,202B5334,extended,24030001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Enhancement patches applied:\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Enable props in World Tour Road Rage mode.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Enable props in Multiplayer/Single Event Road Rage mode.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Enable props in Traffic Attack mode.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Prevent race cars reflections from fading further away.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Falling car parts while driving (takedowns and traffic checks).\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Prevent the game to remove out of range crashing traffic.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Render all extra particles while driving.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Force max LOD cars to 5 (highest level).\n");
			}
			/* Castlevania: Curse of Darkness (NTSC-U) [CRC: 3A446111] */
			else if (!strcmp(serial, "SLUS-21168"))
			{
				static const char *const patches[] = {
					/* draw distance 4x */
					"patch=1,EE,00411bcc,word,00e0102d",
					"patch=1,EE,00411bd0,word,90460020",
					"patch=1,EE,00411bd4,word,44843000",
					"patch=1,EE,00411bd8,word,468031a0",
					"patch=1,EE,00411bdc,word,a04305f2",
					"patch=1,EE,00411be0,word,ac440010",
					"patch=1,EE,00411be4,word,44862000",
					"patch=1,EE,00411be8,word,46802120",
					"patch=1,EE,00411bec,word,00050880",
					"patch=1,EE,00411bf0,word,44810800",
					"patch=1,EE,00411bf4,word,46800860",
					"patch=1,EE,00411bf8,word,45000006",
					"patch=1,EE,00411bfc,word,ac450014",
					"patch=1,EE,00411c00,word,3c013f80",
					"patch=1,EE,00411c04,word,44812800",
					"patch=1,EE,00411c08,word,10000007",
					"patch=1,EE,00411c0c,word,46002007",
					"patch=1,EE,00411c10,word,00000000",
					"patch=1,EE,00411c14,word,46052803",
					"patch=1,EE,00411c18,word,00000000",
					"patch=1,EE,00411c1c,word,00000000",
					"patch=1,EE,00411c20,word,46050143",

					"patch=1,EE,00435908,word,3c01c100",
					"patch=1,EE,007717c4,word,8e020058",
					"patch=1,EE,007717cc,word,00021080"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Castlevania: Curse of Darkness (NTSC-U)]: Draw distance multiplied by 4x patch applied:\n");
			}
			/* Dynasty Warriors 2 (NTSC-U) [CRC: 5B665C0B] */
			else if (!strcmp(serial, "SLUS-20079"))
			{
				static const char *const patches[] = {
					/* Increased Draw distance */
					"patch=1,EE,002bbcdc,word,00000000",
					"patch=1,EE,002bbce0,word,00000000",
					"patch=1,EE,002bbd10,word,00000000",
					"patch=1,EE,002543ac,word,00000000",
					"patch=1,EE,002bbcc4,word,00000000",
					"patch=1,EE,002bbcc0,word,00000000",
					"patch=1,EE,003e018c,word,46C35000",

					"patch=1,EE,E00266ee,extended,103E2EB1",
					"patch=1,EE,203503cc,extended,466a6000",
					"patch=1,EE,203503c8,extended,465ac000",
					"patch=1,EE,E00366ee,extended,003E2EB1",
					"patch=1,EE,202bbcc4,extended,e4c0000c",
					"patch=1,EE,202bbcc0,extended,e4c10008",
					"patch=1,EE,202543ac,extended,0c08a860",

					"patch=1,EE,003503cc,word,469C4000",  /* 11846 */
					"patch=1,EE,003503c8,word,46947000",  /* 11841 */
					/* Model Render Bug Fix */
					"patch=1,EE,00202938,word,3C01427f",
					/* Skip Events With X Button */
					"patch=1,EE,0020E810,word,30424008",
					/* FMV Skip with X button */
					"patch=1,EE,002BAADC,word,30424008",
					/* Able to skip Koei Logo */
					"patch=1,EE,003e22b0,word,002BAAC8",
					/* Pick up items while mounted 
					 * This allows to pick up items while 
					 * on a horse like in DW4/5. */
					"patch=1,EE,00287F70,word,3C01433e",
					/* Disable Distance Based Model Disappearing */
					"patch=1,EE,00230d70,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 2 (NTSC-U)]: Enhancement patches applied:\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 2 (NTSC-U)]: Increased draw distance.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 2 (NTSC-U)]: Model render bugfix.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 2 (NTSC-U)]: Skip events with X button.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 2 (NTSC-U)]: FMV skip with X button.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 2 (NTSC-U)]: Skip Koei logo with X button.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 2 (NTSC-U)]: Pick up items while mounted, allows you to pickup items while on a horse like in Dynasty Warriors 4/5.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 2 (NTSC-U)]: Disable distance based models disappearing.\n");
			}
			/* Dynasty Warriors 4 (NTSC-U) [CRC: 6C89132B] [UNDUB] [CRC: 6C881C2B] */
			else if (!strcmp(serial, "SLUS-20653"))
			{
				static const char *const patches[] = {
					/* Disable Distance Based Model Disappearing */
					"patch=1,EE,001ce0d0,word,00000000",
					/* High LOD */
					"patch=1,EE,0018C8d0,word,00000000",
					"patch=1,EE,0018CE9C,word,00000000",
					/* Skip Events with X Button (DUELS ACCEPT IS SQUARE) */
					"patch=1,EE,0020BB98,word,24034008",
					"patch=1,EE,0020BA94,word,30638000",
					/* FMV Skip with X button */
					"patch=1,EE,002100A4,word,30424008",
					/* Able to Skip Koei Logo */
					"patch=1,EE,00362CEC,word,00210090",
					/* Increase default of 24 max units 
					 * rendered at the same time to 26. */
					"patch=1,EE,001CDFB0,word,2403001a"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4 (NTSC-U)]: Enhancement patches applied:\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4 (NTSC-U)]: High LOD.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4 (NTSC-U)]: Increase default of 24 max units rendered simultaneously to 26.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4 (NTSC-U)]: Skip events with X button (Duels accept is Square button).\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4 (NTSC-U)]: FMV skip with X button.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4 (NTSC-U)]: Skip Koei logo with X button.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4 (NTSC-U)]: Disable distance based models disappearing.\n");
			}
			/* Dynasty Warriors 4: Empires (NTSC-U] [CRC: BD3DBCF9] */
			else if (!strcmp(serial, "SLUS-20938"))
			{
				static const char *const patches[] = {
					/* Increased draw distance Empires */
					"patch=1,EE,0015648C,word,00000000",
					"patch=1,EE,0015643C,word,00000000",
					"patch=1,EE,20508F1C,word,463b8000", /* 1P Mode */
					"patch=1,EE,20508F40,word,463b8000",
					"patch=1,EE,20508F64,word,463b8000",
					"patch=1,EE,20508FAC,word,463b8000",
					"patch=1,EE,20508FD0,word,463b8000",
					"patch=1,EE,20508F18,word,4633b000",
					"patch=1,EE,20508F3c,word,4633b000",
					"patch=1,EE,20508F60,word,4633b000",
					"patch=1,EE,20508FA8,word,4633b000",
					"patch=1,EE,20508Fcc,word,4633b000",
					/* Increases default of 24 maximum units 
					 * rendered at the same time to 28. */
					"patch=1,EE,001cbd34,word,2402001c"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4: Empires (NTSC-U)]: Enhancement patches applied:\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4: Empires (NTSC-U)]: Increased draw distance.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4: Empires (NTSC-U)]: Increases default of 24 maximum units rendered simultaneously to 28.\n");
			}
			/* Grand Theft Auto: San Andreas (NTSC-U) [CRC: 399A49CA] */
			else if (!strcmp(serial, "SLUS-20946"))
			{
				/* Addresses are for the CRC 399A49CA pressing; other
				 * pressings reuse the serial with a different ELF, so
				 * only patch the one these were made for. */
				if (game_crc == 0x399A49CA)
				{
					static const char *const patches[] = {
						/* Enable Hot Coffee */
						"patch=1,EE,206B32FC,extended,00000000",
						"patch=1,EE,E003F0FF,extended,00700942",
						"patch=1,EE,2088D760,extended,4C333132",
						"patch=1,EE,2088D764,extended,3244334C",
						"patch=1,EE,2088D768,extended,32000052"
					};
					LOAD_PATCHES(patches);
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: San Andreas (NTSC-U)]: Hot Coffee enabled.\n");
				}
				else
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: San Andreas (NTSC-U)]: Hot Coffee patch skipped, no patch for this revision (CRC %08X).\n", game_crc);
			}
			/* King of Fighters '98 - Ultimate Match, The (NTSC-U) [CRC: E5A904B3] */
			else if (!strcmp(serial, "SLUS-21816"))
			{
				static const char *const patches[] = {
					/* Enable blood particle effect (e.g. Choi's claw attacks) 
					 * just like the Japanese version, also works 
					 * in NeoGeo mode */
					"patch=1,EE,00327da4,word,a0400000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [King of Fighters '98: Ultimate Match, The (NTSC-U)]: Enable bood particle effect (e.g. Choi's claw attacks, just like the Japanese version, also works in NeoGeo mode).\n");
			}
			/* King of Fighters 2000, The (NTSC-U) [CRC: AED59B8E] */
			else if (!strcmp(serial, "SLUS-20834"))
			{
				static const char *const patches[] = {
					/* Uncensored version */
					"patch=1,EE,00152cf0,word,93838a68",
					"patch=1,EE,00152d04,word,93828a8c",
					"patch=1,EE,00152d14,word,9062133e",
					/* Whip's original desert eagle ending */
					"patch=1,EE,002f97b4,word,90443ee6"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [King of Fighters 2000, The (NTSC-U)]: Uncensored version patch applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [King of Fighters 2000, The (NTSC-U)]: Whip's original Desert Eagle ending patch applied.\n");
			}
			/* Le Mans 24 Hours (NTSC-U) [CRC: 67835861] */
			else if (!strcmp(serial, "SLUS-20207"))
			{
				static const char *const patches[] = {
					/* Disable far objects culling (enhance draw distance) */
					"patch=1,EE,00317F40,double,03e00008"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Le Mans 24 Hours (NTSC-U)]: Enhanced draw distance patch applied (disable far objects culling).\n");
			}
			/* SSX Tricky (NTSC-U) [CRC: 8E7CFF62] */
			else if (!strcmp(serial, "SLUS-20326"))
			{
				static const char *const patches[] = {
					/* Disable Character LOD control */
					"patch=1,EE,00122028,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [SSX Tricky (NTSC-U)]: Disable character LOD control patch applied.\n");
			}
			/* SSX3 (NTSC-U) [CRC: 08FFF00D] */
			else if (!strcmp(serial, "SLUS-20772"))
			{
				static const char *const patches[] = {
					/* Disable intro videos (ea / thx / splash) */
					"patch=0,EE,001A2840,word,0000202D",
					"patch=0,EE,001A2864,word,0000202D",
					"patch=0,EE,001A28DC,word,0000202D"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [SSX3 (NTSC-U)]: Disable intro videos (EA/THX/splash).\n");
			}
			/* SSX On Tour (NTSC-U) [CRC: 0F27ED9B] */
			else if (!strcmp(serial, "SLUS-21278"))
			{
				/* Patch courtesy: Zenloup */
				static const char *const patches[] = {
					/* LOD Control
					 * 1=Highpoly/cutscenes 2=Medium Poly 3= Lowpoly */
					"patch=1,EE,0011BA84,word,1"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [SSX On Tour (NTSC-U)]: LOD Control set to Highpoly patch applied.\n");
			}
			/* Tokyo Xtreme Racer 3 (NTSC-U) [CRC: 0F932D81] */
			else if (!strcmp(serial, "SLUS-20831"))
			{
				static const char *const patches[] = {
					/* Increased draw distance Conquest */
					"patch=1,EE,00889E4A,word,00004E20",
					/* Increased draw distance Other Modes */
					"patch=1,EE,00873F2A,word,00004E20",
					/* Increased draw distance Rival Battle */
					"patch=1,EE,0088020A,word,00004E20"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tokyo Xtreme Racer 3 (NTSC-U)]: Enhancement patches applied:\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tokyo Xtreme Racer 3 (NTSC-U)]: Increased draw distance (Conquest mode).\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tokyo Xtreme Racer 3 (NTSC-U)]: Increased draw distance (Other modes).\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tokyo Xtreme Racer 3 (NTSC-U)]: Increased draw distance (Rival Battle mode).\n");
			}
		}
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* Dark Cloud (NTSC-U) [CRC: A5C05C78] */
			if (!strcmp(serial, "SCUS-97111"))
			{
				static const char *const patches[] = {
					/* CNP Draw Distance */
					"patch=1,EE,00156554,word,00000000",
					"patch=1,EE,001729DC,word,00000000",
					"patch=1,EE,00155FF0,word,00000000",
					/* LOD Distance */
					"patch=1,EE,00157364,word,10000006",
					/* Shade */
					"patch=1,EE,001A3E80,word,00000000",
					"patch=1,EE,001A3D80,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Cloud (NTSC-U)]: Draw distance/LOD enhancement patch applied.\n");
			}
			/* Downhill Domination (NTSC-U) [CRC: 5AE01D98] */
			else if (!strcmp(serial, "SCUS-97177"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0029DAA8,word,00000000" /* Max LOD Distance */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Downhill Domination (NTSC-U)]: Max LOD distance enhancement patch applied.\n");
			}
			/* God of War II (NTSC-U) [CRC: 2F123FD8] */
			else if (!strcmp(serial, "SCUS-97481"))
			{
				static const char *const patches[] = {
					/* Allow MPEG skip by pressing x */
					"patch=1,EE,001DD8C8,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [God of War II (NTSC-U)]: Allow skipping cutscenes enhancement patch applied.\n");
			}
			/* Gran Turismo 4 (NTSC-U) [CRC: 77E61C8A] */
			else if (!strcmp(serial, "SCUS-97328"))
			{
				static const char *const patches[] = {
					/* Max LOD cars */
					"patch=1,EE,204539C0,extended,10000009",
					"patch=1,EE,20454FBC,extended,1000000E"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gran Turismo 4 (NTSC-U)]: Max LOD cars enhancement patch applied.\n");
			}
		}
		else if (!strncmp("SCPS-", serial, strlen("SCPS-")))
		{
			/* Gran Turismo 3 A-Spec (NTSC-J) [CRC: 9DE5CF65] */
			if (!strcmp(serial, "SCPS-15009"))
			{
				static const char *const patches[] = {
					/* Max car LODs */
					"patch=1,EE,21BD8A,short,1000",
					"patch=1,EE,21CA16,short,1000",
					"patch=1,EE,21F2E2,short,1000",
					"patch=1,EE,2212A2,short,1000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gran Turismo 3 (NTSC-J)]: Max car LODs patch applied.\n");
			}
			/* Gran Turismo 4 Prologue (NTSC-J) [CRC: EF258742] */
			else if (!strcmp(serial, "SCPS-15055"))
			{
				static const char *const patches[] = {
					/* car higher LOD - higher LOD wheels */
					"patch=1,EE,2057702C,extended,756E656D",
					"patch=1,EE,00577030,extended,0000002F",
					"patch=1,EE,2055C344,extended,6E656D2F",
					"patch=1,EE,2055C348,extended,73252F75"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gran Turismo 4 (NTSC-J)]: Max car LODs patch applied.\n");
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Sega Rally 2006 (NTSC-J) [CRC: B26172F0] */
			if (!strcmp(serial, "SLPM-66212"))
			{
				static const char *const patches[] = {
					/* Render Distance Patch (required, adds +25%) */
					"patch=1,EE,2017B150,extended,00000000", 
					/* +100% Render Distance (0.35f, max without glitching) */
					"patch=1,EE,203832EC,word,3EB33333"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sega Rally 2006 (NTSC-J)]: Increased draw distance (125%%) patch applied.\n");
			}
			/* Tokyo Bus Annai 2 (NTSC-J) */
			else if (!strcmp(serial, "SLPM-65982"))
			{
				/* Patch courtesy: kozarovv */
				static const char *const patches[] = {
					/* Mirrors draw distance, and max details */
					"patch=1,EE,0019FE48,word,00000000",
					"patch=1,EE,0019B988,word,3C023F80",
					"patch=1,EE,0019B98C,word,00000000",
					/* World draw distance */
					"patch=1,EE,01D83164,word,45000000",
					"patch=1,EE,01D83168,word,45000000",
					"patch=1,EE,01D83174,word,45000000",
					"patch=1,EE,01D83178,word,45000000",
					"patch=1,EE,01D8317C,word,45000000",
					"patch=1,EE,01D83188,word,45000000",
					"patch=1,EE,01D83190,word,45000000",

					"patch=1,EE,001A09DC,word,00000000",
					"patch=1,EE,01D8316C,word,45000000",
					"patch=1,EE,001A09C0,word,3C034500",
					"patch=1,EE,01D05B00,word,45000000",
					"patch=1,EE,0017AD10,word,00000000",
					/* Trees - problematic at higher value */
					"patch=1,EE,01D83170,word,43fc0000",
					/* People draw distance - Last patch do real job,
					 * sadly it break one of stages even at little bit higher value. */
					"patch=1,EE,001B3F4C,word,00000000",
					"patch=1,EE,001B3F54,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tokyo Bus Annai 2 (NTSC-J)]: Enhancement patches applied:\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tokyo Bus Annai 2 (NTSC-J)]: Increased world draw distance.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tokyo Bus Annai 2 (NTSC-J)]: Increased NPC draw distance.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tokyo Bus Annai 2 (NTSC-J)]: Increased mirrors draw distance, and max details.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tokyo Bus Annai 2 (NTSC-J)]: Increased trees.\n");
			}
		}
	}

	if (hint_uncapped_framerate > 0)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* 24 - The Game (NTSC-U) [CRC: F1C7201E] */
			if (!strcmp(serial, "SLUS-21268"))
			{
				/* Patch courtesy: Red-tv */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					/* 60fps */
					"patch=1,EE,005F9808,word,00000001",
					/* Fix FMV */
					"patch=1,EE,e0010001,extended,0058EEF4",
					"patch=1,EE,205F9808,extended,00000002"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [24: The Game (NTSC-U)]: 60fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Aeon Flux (NTSC-U) [CRC: 9FA0A1B0] */
			else if (!strcmp(serial, "SLUS-21205"))
			{
				/* 60fps uncapped. Need EE Overclock at 300%. */
				static const char *const patches[] = {
					"patch=1,EE,2036C438,extended,28630001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Aeon Flux (NTSC-U)]: 60fps patch applied (needs 300%% EE cyclerate).\n");
			}
			/* Alias (NTSC-U) [CRC: E3ADDC73] */
			else if (!strcmp(serial, "SLUS-20673"))
			{
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,2024BEF0,extended,14400039" /* 10400039 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Alias (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Baroque (NTSC-U) [CRC: 4566213C] */
			else if (!strcmp(serial, "SLUS-21714"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,00556E70,word,00000000",
					/* Revert to 30fps in FMV and cutscenes */
					"patch=1,EE,e0010000,extended,005179C0",
					"patch=1,EE,20556E70,extended,00000001",
					/* Player Speed Modifier */
					"patch=1,EE,0013D770,word,3C033F00",
					"patch=1,EE,00143CA4,word,3C023F00",
					"patch=1,EE,00146FEC,word,3C033F00",
					/* Enemy and NPC Animation Speed Modifier */
					"patch=1,EE,00146E08,word,3C023f00",
					"patch=1,EE,00146DF0,word,3C033eCC",
					/* Camera Speed Modifier */
					"patch=1,EE,0013DCBC,word,3C023F80",
					/* Player's Gauge Speed Modifier */
					"patch=1,EE,001341d8,word,3c024000",
					"patch=1,EE,00133ff4,word,3c024000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Baroque (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Batman - Rise of Sin Tzu (NTSC-U) [CRC: 24280F22] */
			else if (!strcmp(serial, "SLUS-20709"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. */
				static const char *const patches[] = {
					"patch=1,EE,00534720,word,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Batman: Rise of Sin Tzu (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Black (NTSC-U) [CRC: 5C891FF1] */
			else if (!strcmp(serial, "SLUS-21376"))
			{
				static const char *const patches[] = {
					"patch=1,EE,1040DF74,extended,00000001", /* 60 fps */
					"patch=1,EE,205A8A9C,extended,3C888889", /* speed */
					"patch=1,EE,204BC13C,extended,3C888889",
					"patch=1,EE,2040EBAC,extended,3C888889"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Black (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Burnout 3: Takedown (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21050"))
			{
				static const char *const patches[] = {
					/* Enable 60fps in menus */
					"patch=0,EE,201D3F2C,extended,1000000A",
					/* Fix FMVs playback speed while using 60 FPS patches */
					"patch=0,EE,20130DD8,extended,C7958074",
					"patch=0,EE,20130DDC,extended,3C084000",
					"patch=0,EE,20130DE0,extended,4488A000",
					"patch=0,EE,20130DE4,extended,4614AD03",
					"patch=0,EE,20130DE8,extended,00000000",

					/* Enable 60fps in crashes */
					"patch=0,EE,201320D8,extended,1000004B",
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: 60fps patch for menus applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout 3: Takedown (NTSC-U)]: 60fps patch for replays applied.\n");
			}
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				static const char *const patches[] = {
					/* 60fps Split Screen */
					"patch=1,EE,20104BC0,extended,080680A0",
					"patch=1,EE,20104BC4,extended,00000000",
					/* 60 FPS Front End */
					"patch=1,EE,201125F4,word,24040001",
					"patch=1,EE,201125EC,word,00108002",
					/* 60 FPS Crashes & Crash Mode */
					"patch=1,EE,20104B9C,word,90850608"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: 60fps patch for splitscreen applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: 60fps patch for menu applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: 60fps patch for crashes and crash mode applied.\n");
			}
			/* Cold Fear (NTSC-U) [CRC: ECFBAB36] */
			else if (!strcmp(serial, "SLUS-21047"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0046E484,extended,00000001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Cold Fear (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Cold Winter (NTSC-U) [CRC: D6D704BB] */
			else if (!strcmp(serial, "SLUS-20845"))
			{
				static const char *const patches[] = {
					"patch=0,EE,2042A534,extended,24630000",
					"patch=0,EE,2042A548,extended,24840000",
					"patch=0,EE,203C0018,extended,080F000C",
					"patch=0,EE,201FE694,extended,24020001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Cold Winter (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Dark Angel - James Cameron's (NTSC-U) [CRC: 29BA2F04] */
			else if (!strcmp(serial, "SLUS-20379"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130% */
				static const char *const patches[] = {
					"patch=1,EE,0027F154,word,10400012"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Angel: James Cameron's (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Dawn of Mana (NTSC-U) [CRC: 9DC6EE5A] */
			else if (!strcmp(serial, "SLUS-21574"))
			{
				/* 60fps uncapped. */
				static const char *const patches[] = {
					/* 28620002 fps without doubling speed */
					"patch=1,EE,20113010,extended,28620001",
					/* condition to avoid hang and skip FMVs */
					"patch=1,EE,E0010001,extended,005D7338", 
					"patch=1,EE,20113010,extended,28620002"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dawn of Mana (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Destroy All Humans! (NTSC-U) [CRC: 67A29886] */
			else if (!strcmp(serial, "SLUS-20945"))
			{
				/* 60fps uncapped. */
				static const char *const patches[] = {
					"patch=1,EE,203EF80C,extended,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Destroy All Humans! (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Deus Ex: The Conspiracy (NTSC) [CRC: 3AD6CF7E] */
			else if (!strcmp(serial, "SLUS-20111"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock to be stable. */
				static const char *const patches[] = {
					"patch=1,EE,2030D234,word,28420001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Deus Ex: The Conspiracy (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Echo Night - Beyond (NTSC) [CRC: 2DE16D21] */
			else if (!strcmp(serial, "SLUS-20928"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,2013FFDC,word,10000014"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Echo Night: Beyond (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Fatal Frame II: Crimson Butterfly (NTSC-U) [CRC: 9A51B627] */
			else if (!strcmp(serial, "SLUS-20766"))
			{
				/* Patch courtesy: Gabominated, asasega */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,2021b7dc,extended,24020000", /* 24020001 */
					"patch=1,EE,201E2BD0,extended,00000000", /* 0C07925A */
					"patch=1,EE,201b2210,extended,3c014148", /* 3c0141c8 */
					"patch=1,EE,201F798C,extended,2C42003c", /* 2C42001E */
					"patch=1,EE,E0040001,extended,002BCF58",
					"patch=1,EE,2021b7dc,extended,24020001",
					"patch=1,EE,201E2BD0,extended,0C07925A",
					"patch=1,EE,201b2210,extended,3c0141c8",
					"patch=1,EE,201F798C,extended,2C42001E",
					"patch=1,EE,E0010001,extended,002E4E44",
					"patch=1,EE,2021b7dc,extended,24020001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Fatal Frame II: Crimson Butterfly (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Grand Theft Auto III (NTSC-U) [CRC: 5E115FB6] */
			else if (!strcmp(serial, "SLUS-20062"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,2027CEAC,extended,28420001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto III (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Grand Theft Auto: Vice City (NTSC-U) [CRC: 20B19E49] */
			else if (!strcmp(serial, "SLUS-20552"))
			{
				/* The addresses below were derived from the CRC 20B19E49
				 * executable. Later pressings reuse the serial with a
				 * different ELF (e.g. v4.00), where these pokes land in
				 * unrelated code and hang the game after the first load,
				 * so only apply them to the revision they were made for. */
				if (game_crc == 0x20B19E49)
				{
					/* 60fps uncapped */
					static const char *const patches[] = {
						"patch=1,EE,20272204,extended,28420001"
					};
					LOAD_PATCHES(patches);
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: Vice City (NTSC-U)]: 60fps patch applied.\n");
				}
				else if (game_crc == 0x248E6126)
				{
					/* v4.00 pressing: the same poke against its vsync-wait
					 * (lw v0,-0x12e4(gp); slti v0,v0,2; bnez), relocated from
					 * 272204 to 277B8C in this ELF. */
					static const char *const patches_v400[] = {
						"patch=1,EE,20277B8C,extended,28420001"
					};
					LOAD_PATCHES(patches_v400);
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: Vice City (NTSC-U) v4.00]: 60fps patch applied.\n");
				}
				else
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: Vice City (NTSC-U)]: 60fps patch skipped, no patch for this revision (CRC %08X).\n", game_crc);
			}
			/* Grand Theft Auto: San Andreas (NTSC-U) [CRC: 399A49CA] */
			else if (!strcmp(serial, "SLUS-20946"))
			{
				/* Same revision guard as the Hot Coffee entry: these
				 * addresses are only valid for the CRC 399A49CA ELF
				 * (verified against it: 39B53C holds li a0,2). */
				if (game_crc == 0x399A49CA)
				{
					/* 60fps uncapped. Need EE Overclock at 180%.
					 * A fourth line, "patch=1,EE,D066804C,word,10000001",
					 * was dropped: a raw-format D-conditional transcribed
					 * with type word, making it a literal read/write of
					 * unmapped 0xD066804C every frame. Measured inert (the
					 * unmapped access is absorbed and every other poke in
					 * the set still lands), but it only works by leaning on
					 * that absorption, and as the conditional it was meant
					 * to be it merely re-tested the flag the line above
					 * writes. */
					static const char *const patches[] = {
						"patch=0,EE,2039B53C,extended,24040001", /* Set VSync Mode to 60 FPS */
						"patch=1,EE,0066804C,word,10000001",
						"patch=1,EE,006678CC,extended,00000001" /* Framerate boost */
					};
					LOAD_PATCHES(patches);
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: San Andreas (NTSC-U)]: 60fps patch applied (needs 180%% EE cyclerate).\n");
				}
				else
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: San Andreas (NTSC-U)]: 60fps patch skipped, no patch for this revision (CRC %08X).\n", game_crc);
			}
			/* Jurassic: The Hunted (NTSC-U) [CRC:EFE4448F] */
			else if (!strcmp(serial, "SLUS-21907"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,2017D480,word,2C420001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Jurassic: The Hunted (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Kingdom Hearts 2 (NTSC-U) [CRC: DA0535FD] */
			else if (!strcmp(serial, "SLUS-21005"))
			{
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,00356F4C,extended,00000000",
					"patch=1,EE,E0010005,extended,0033E784",
					"patch=1,EE,00356F4C,extended,00000001",
					"patch=1,EE,20379178,extended,3F800000",
					"patch=1,EE,2037CE98,extended,3F800000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Kingdom Hearts II (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Lord of the Rings, Return of the King (NTSC-U) [CRC: 4CE187F6] */
			else if (!strcmp(serial, "SLUS-20770"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,2014B768,extended,10000013" /* 14400003 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Lord of the Rings, Return of the King (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Matrix, The - Path of Neo (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-21273"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,00463E1C,word,3F800000",
					"patch=1,EE,00463E2C,word,42700000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Matrix, The - Path of Neo (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Max Payne 2: The Fall of Max Payne (NTSC-U) [CRC: CD68E44A] */
			else if (!strcmp(serial, "SLUS-20814"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,005D8DF8,word,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Max Payne 2: The Fall of Max Payne (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Metal Gear Solid 2: Substance (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20554"))
			{
				/* Patch courtesy: flcl8193 */
				/* 60fps uncapped cutscenes. */
				static const char *const patches[] = {
					"patch=1,EE,001914F4,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Metal Gear Solid 2: Substance (NTSC-U)]: 60fps uncapped cutscenes patch applied.\n");
			}
			/* Metal Gear Solid 3: Subsistence (NTSC-U) (Disc 1) [CRC: ] */
			else if (!strcmp(serial, "SLUS-21359"))
			{
				/* Patch courtesy: felixthecat1970 */
				static const char *const patches[] = {
					/* override FPS lock functions calls, 
					 * disable others FPS codes */
					"patch=1,EE,20145830,extended,0C03FFE8",
					"patch=1,EE,200FFFA0,extended,241B0001",
					"patch=1,EE,200FFFA4,extended,145B0008",
					"patch=1,EE,200FFFA8,extended,00000000",
					"patch=1,EE,200FFFAC,extended,149B0006",
					"patch=1,EE,200FFFB0,extended,00000000",
					"patch=1,EE,200FFFB4,extended,161B0004",
					"patch=1,EE,200FFFB8,extended,00000000",
					"patch=1,EE,200FFFBC,extended,0000102D",
					"patch=1,EE,200FFFC0,extended,0000202D",
					"patch=1,EE,200FFFC4,extended,0000802D",
					"patch=1,EE,200FFFC8,extended,03E00008",
					"patch=1,EE,20145570,extended,24060001",
					"patch=1,EE,201453B4,extended,240B0001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Metal Gear Solid 3: Subsistence (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Metal Arms - Glitch in the System (NTSC-U) [CRC: E8C504C8] */
			else if (!strcmp(serial, "SLUS-20786"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					"patch=1,EE,004B2C98,word,00000001" /* 00000002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Metal Arms - Glitch in the System (NTSC-U)]: 60fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Midnight Club - Street Racing (NTSC-U) */
			else if (!strcmp(serial, "SLUS-20063"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,20302934,word,00000001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Midnight Club: Street Racing (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Midnight Club II (NTSC-U) */
			else if (!strcmp(serial, "SLUS-20209"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,2042FAB8,word,00000001", /* fps */
					"patch=1,EE,20432164,word,3C888889" /* speed */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Midnight Club II (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Midnight Club 3 - DUB Edition (NTSC-U) v1.0 [CRC: 4A0E5B3A] */
			else if (!strcmp(serial, "SLUS-21029"))
			{
				/* 60fps uncapped. Need EE Overclock at 180%. */
				if (     game_crc == 0x0DD3417A) /* 2.00 */
				{
					static const char *const patches[] = {
						"patch=1,EE,00617F30,word,00000001" /* 00000002 */
					};
					LOAD_PATCHES(patches);
				}
				else
				{
					static const char *const patches[] = {
						"patch=1,EE,00617AB4,word,00000001" /* 00000002 */
					};
					LOAD_PATCHES(patches);
				}
				log_cb(RETRO_LOG_INFO, "[PATCH] [Midnight Club 3: DUB Edition (NTSC-U)]: 60fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Need For Speed - Hot Pursuit 2 (NTSC-U) [CRC: 1D2818AF] */
			else if (!strcmp(serial, "SLUS-20362"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=0,EE,0032F638,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Need For Speed: Hot Pursuit 2 (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Need For Speed Underground 1 (NTSC-U) [CRC: CB99CD12] */
			else if (!strcmp(serial, "SLUS-20811"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,2011060C,word,2C420001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Need For Speed Underground (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Need For Speed Underground 2 (NTSC-U) [CRC: F5C7B45F] */
			else if (!strcmp(serial, "SLUS-21065"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,201D7ED4,word,2C420001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Need For Speed Underground 2 (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Power Rangers - Dino Thunder (NTSC-U) [CRC: FCD89DC3] */
			else if (!strcmp(serial, "SLUS-20944"))
			{
				/* Patch courtesy: felixthecat1970 */
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=0,EE,101400D4,extended,2403003C",
					"patch=0,EE,2020A6BC,extended,241B0001",
					"patch=0,EE,2020A6C4,extended,03E00008",
					"patch=0,EE,2020A6C8,extended,A39B8520",
					"patch=0,EE,2020A7A8,extended,241B0002",
					"patch=0,EE,2020A7F8,extended,A39B8520"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Power Rangers: Dino Thunder (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Prince of Persia: The Sands of Time (NTSC-U) [CRC: 7F6EB3D0] */
			else if (!strcmp(serial, "SLUS-20743"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					"patch=1,EE,0066D044,word,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Prince of Persia: The Sands of Time (NTSC-U)]: 60fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Project - Snowblind (NTSC-U) [CRC: 2BDA8ADB] */
			else if (!strcmp(serial, "SLUS-21037"))
			{
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,002d4c04,word,2c620000" /* 0062102B */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Project Snowblind (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Psi-Ops: The Mindgate Conspiracy (NTSC-U) [CRC: 9C71B59E] */
			else if (!strcmp(serial, "SLUS-20688"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,2017AB28,extended,00000000" /* 1640FFE5 fps1 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Psi-Ops: The Mindgate Conspiracy (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Red Faction (NTSC-U) [CRC: FBF28175] */
			else if (!strcmp(serial, "SLUS-20073"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,20164F9C,extended,24040001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Red Faction (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Red Faction II (NTSC-U) [CRC: 8E7FF6F8] */
			else if (!strcmp(serial, "SLUS-20442"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,201218A0,word,24040001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Red Faction II (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Red Ninja: End of Honor (NTSC-U) [CRC: 6B0F338D] */
			else if (!strcmp(serial, "SLUS-20714"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,E0030002,extended,0047b210",
					"patch=1,EE,2047b210,extended,00000001", /* 00000002 //fps */
					"patch=1,EE,205210a8,extended,3f800000", /* 40000000 //speed modifiers */
					"patch=1,EE,2015b274,extended,3c013f40", /* 3c013f80 //environment speed */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Red Ninja: End of Honor (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Reign of Fire (NTSC-U) [CRC: D10945CE] */
			else if (!strcmp(serial, "SLUS-20556"))
			{
				/* Patch courtesy: Gabominated */
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,00264E70,word,00000001", /* 00000002 */
					"patch=1,EE,001409b4,word,2402003c"  /* 2402001e native global speed */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Reign of Fire (NTSC-U)]: 60fps patch applied.\n");

			}
			/* Return to Castle Wolfenstein: Operation Resurrection (NTSC-U) [CRC: 5F4DB1DD] */
			else if (!strcmp(serial, "SLUS-20297"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=0,EE,2017437C,word,2C420001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Return to Castle Wolfenstein: Operation Resurrection (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Rune - Viking Warlord (NTSC-U) [CRC: 1259612B] */
			else if (!strcmp(serial, "SLUS-20109"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					"patch=1,EE,001305A4,extended,28420001" /* 28420002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Rune: Viking Warlord (NTSC-U)]: 60fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Scarface - The World is Yours (NTSC-U) [CRC: 41F4A178] */
			else if (!strcmp(serial, "SLUS-21111"))
			{
				/* 60fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					"patch=1,EE,20DAFABC,word,00000000" /* 00000001 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Scarface: The World Is Yours (NTSC-U)]: 60fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Serious Sam - Next Encounter (NTSC-U) [CRC: 155466E8] */
			else if (!strcmp(serial, "SLUS-20907"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=0,EE,20127580,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Serious Sam: Next Encounter (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Sitting Ducks (NTSC-U) [CRC: 76A65B01] */
			else if (!strcmp(serial, "SLUS-20886"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. */
				static const char *const patches[] = {
					"patch=1,EE,0067FF58,extended,00000032",
					"patch=1,EE,E0010001,extended,00469D64",
					"patch=1,EE,0067FF58,extended,00000019"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sitting Ducks (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Sonic Heroes (NTSC-U) [CRC: 78FF4E3B] */
			else if (!strcmp(serial, "SLUS-20718"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped. */
				static const char *const patches[] = {
					"patch=1,EE,004777C0,word,00000001", /* fps */
					"patch=1,EE,2028FF5C,word,24020001" /* speed */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sonic Heroes (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Spawn - Armageddon (NTSC-U) [CRC: B7E7D66F] */
			else if (!strcmp(serial, "SLUS-20707"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=0,EE,00226830,word,24020001" /* 24020002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Spawn: Armageddon (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Spider-Man - Friend or Foe (NTSC-U) [CRC: F52477F7] */
			else if (!strcmp(serial, "SLUS-21600"))
			{
				/* Patch courtesy: Gabominated */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,2037DCA0,extended,00000001" /* 00000002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Spiderman: Friend or Foe (NTSC-U)]: 60fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Splinter Cell - Pandora Tomorrow (NTSC-U) [CRC: 0277247B] */
			else if (!strcmp(serial, "SLUS-20958"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,0018D778,word,24030001" /* 24030002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Splinter Cell: Pandora Tomorrow (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* SSX3 (NTSC-U) [CRC: 08FFF00D] */
			else if (!strcmp(serial, "SLUS-20772"))
			{
				static const char *const patches[] = {
					/* Disable perf frame skip (metro slowdown) */
					"patch=0,EE,00230704,word,00000000",
					"patch=0,EE,00230710,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [SSX3 (NTSC-U)]: Disable performance frameksip (Metro slowdown fixed) patch applied.\n");
			}
			/* SSX On Tour (NTSC-U) [CRC: 0F27ED9B] */
			else if (!strcmp(serial, "SLUS-21278"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				static const char *const patches[] = {
					/* Forces the FrameHalver variable to 1
					 * 1 = 60fps, 2 = 30fps, and probably 3 = 15fps. */
					"patch=1,EE,003132b4,extended,01001124",
					/* Skipping some nonsense code that's probably 
					 * no longer needed */
					"patch=1,EE,003132b8,extended,15000010"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [SSX On Tour (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Summoner 2 (NTSC-U) [CRC: 93551583] */
			else if (!strcmp(serial, "SLUS-20448"))
			{
				/* Patch courtesy: asasega */
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=0,EE,2017BC34,word,24040001" /* 60fps */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Summoner 2 (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Super Monkey Ball Deluxe (NTSC-U) [CRC: 43B1CD7F] */
			else if (!strcmp(serial, "SLUS-20918"))
			{
				/* Patch courtesy: gamehacking.org, by Josh_7774, & Gabominated, PCSX2 forum */
				/* 60fps uncapped. Breaks Golf & Tennis. */
				static const char *const patches[] = {
					"patch=1,EE,20146D04,extended,24020001",
					/* Following patches fixes FMVs */
					"patch=1,EE,004C318C,extended,00000001", 
					"patch=1,EE,E0010001,extended,00473478",
					"patch=1,EE,204C318C,extended,00000002"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Super Monkey Ball Deluxe (NTSC-U)]: 60fps patch applied. Breaks Golf & Tennis.\n");
			}
			/* Star Wars - The Force Unleashed (NTSC-U) [CRC: 879CDA5E] */
			else if (!strcmp(serial, "SLUS-21614"))
			{
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,00125948,word,28420001",  /* 28420002 */
					"patch=1,EE,E0010000,extended,01FFFA70",
					"patch=1,EE,00125948,extended,28420002"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Star Wars: The Force Unleashed (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Unreal Tournament (NTSC-U) [CRC: 5751CAC1] */
			else if (!strcmp(serial, "SLUS-20034"))
			{
				/* 60fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					"patch=1,EE,0012D134,extended,28420001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Unreal Tournament (NTSC-U)]: 60fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* XGRA - Extreme G Racing Association (NTSC-U) [CRC: 56B36513] */
			else if (!strcmp(serial, "SLUS-20632"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,002052B4,extended,30420004",
					"patch=1,EE,E0010000,extended,01FFE32C",
					"patch=1,EE,002052B4,extended,30420008"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [XGRA: Extreme G Racing Association (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
		}
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* Dark Cloud 2 (NTSC-U) [CRC: 1DF41F33] */
			if (!strcmp(serial, "SCUS-97213"))
			{
				/* 60fps uncapped. Need EE Overclock at 130% */
				static const char *const patches[] = {
					"patch=1,EE,00376C50,word,00000001", /* 60fps */
					/* Animation Speed Modifier */
					"patch=1,EE,00174698,word,3c033f00", /* 3c033f80 */
					/* Fix Character isn't walking */
					"patch=1,EE,001746e0,word,3c033F00", /* 3c033f80 */
					/* Revert to 30FPS during ingame cutscenes */
					"patch=1,EE,E004CCCC,extended,10381134",
					"patch=1,EE,E0030000,extended,01ECE40C",
					"patch=1,EE,20376C50,extended,00000002",
					"patch=1,EE,20174698,extended,3C033F80",
					"patch=1,EE,201746e0,extended,3C033F80",
					/* Fix Player Jumps too far */
					"patch=1,EE,003560c8,word,3f000000" /* 3f800000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Cloud 2 (NTSC-U)]: 60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* MotorStorm - Arctic Edge (U)(SCUS-97654) */
			else if (!strcmp(serial, "SCUS-97654"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,2039BAF8,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [MotorStorm: Arctic Edge (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Primal (NTSC-U) [CRC: FCD89DC3] */
			else if (!strcmp(serial, "SCUS-97142"))
			{
				/* 60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,204874FC,word,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Primal (NTSC-U)]: 60fps patch applied.\n");
			}
			/* Rise of the Kasai (NTSC-U) [CRC: EDE17E1B] */
			else if (!strcmp(serial, "SCUS-97416"))
			{
				/* Patch courtesy: Gabominated */
				/* 60fps uncapped. */
				static const char *const patches[] = {
					"patch=1,EE,00345A00,word,10A00003" /* 14A00003 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Rise of the Kasai (NTSC-U)]: 60fps patch applied.\n");
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* 7 Blades (PAL-M) */
			if (!strcmp(serial, "SLES-50109"))
			{
				/* Patch courtesy: Gabominated */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,203BE838,extended,24020000", /* 24020001 fps */
					"patch=1,EE,2051bb2c,extended,3E99999a", /* 3F19999a speed */
					"patch=1,EE,2035e8e8,extended,3c013f00", /* 3c013f80 map A */
					"patch=1,EE,2036565c,extended,3c013f00"  /* 3c013f80 map B */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [7 Blades (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* 7 Sins (PAL-M) [CRC: 52DEB87B] TODO/FIXME - might not work */
			else if (!strcmp(serial, "SLES-53280") || !strcmp(serial, "SLES-53297"))
			{
				switch (hint_uncapped_framerate)
				{
					case 2: /* 60fps NTSC */
						{
							/* Patch courtesy: Gabominated */
							/* 60fps uncapped. Need EE Overclock at 130%. */
							static const char *const patches[] = {
								"patch=1,EE,001008f4,word,240201c0" /* 24020200 */
							};
							LOAD_PATCHES(patches);
							set_system_av_info = 1;
						}
						break;
					default:
						{
							/* Patch courtesy: Gabominated */
							/* 50fps uncapped. Need EE Overclock at 130%. */
							static const char *const patches[] = {
								"patch=1,EE,00428390,word,24020002" /* 24020001 */
							};
							LOAD_PATCHES(patches);
						}
						break;
				}
				log_cb(RETRO_LOG_INFO, "[PATCH] [7 Sins (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Aeon Flux (PAL-M) [CRC: 761CABB3] */
			else if (!strcmp(serial, "SLES-54169"))
			{
				/* 50fps uncapped. Need EE Overclock at 300%. */
				static const char *const patches[] = {
					"patch=1,EE,00362EB8,word,28630001" /* 28630002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Aeon Flux (PAL)]: 50fps patch applied (needs 300%% EE cyclerate).\n");
			}
			/* Alias (PAL-M) [CRC: 83466553] */
			if (!strcmp(serial, "SLES-51821"))
			{
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,0024BEAC,word,2C420000", /* 2C42001E */
					"patch=1,EE,001DED08,word,3C013F00"  /* 3C013F80 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Alias (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Dark Angel (PAL) [CRC: 5BE3F481] */
			else if (!strcmp(serial, "SLES-51333"))
			{
				/* Patch courtesy: PeterDelta */
				/* Uncapped. Need EE Overclock at 130% */
				static const char *const patches[] = {
					"patch=1,EE,00280B74,word,1040000D"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Angel (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Echo Night - Beyond (PAL) [CRC: BBF8C3D6] */
			else if (!strcmp(serial, "SLES-53414"))
			{
				/* Patch courtesy: PeterDelta */
				/* 60fps uncapped. Need EE Overclock at 130%. Select 60Hz */
				static const char *const patches[] = {
					"patch=1,EE,E001001E,extended,0028A348",
					"patch=1,EE,0028A348,extended,0000003C"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Echo Night: Beyond (PAL)]: 50/60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* London Racer World Challenge (PAL-M) [CRC: F97680AA] */
			else if (!strcmp(serial, "SLES-51580"))
			{
				/* Patch courtesy: Gabominated */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,00133810,word,24020000" /* 24020001 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [London Racer World Challenge (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Metal Arms - Glitch in the System (PAL) [CRC: AF399CCC] */
			else if (!strcmp(serial, "SLES-51758"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					"patch=1,EE,004BEA90,word,00000001" /* 00000002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Metal Arms: Glitch in the System (PAL)]: 50fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Metal Gear Solid 2: Substance (PAL-M) [CRC: 093E7D52] */
			else if (!strcmp(serial, "SLES-82009"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped cutscenes. */
				static const char *const patches[] = {
					"patch=1,EE,E0010002,extended,00191A34",
					"patch=1,EE,00191A34,extended,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Metal Gear Solid 2: Substance (PAL)]: 50fps uncapped cutscenes patch applied.\n");
			}
			/* Project Zero 2 - Crimson Butterfly (PAL) [CRC: 9D87F3AF] */
			else if (!strcmp(serial, "SLES-52384"))
			{
				/* Patch courtesy: Gabominated, asasega */
				/* 50/60fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,2022088C,extended,00000000", /* 8F82C960 */
					"patch=1,EE,201E6028,extended,00000000", /* 0C079FB0 */
					"patch=1,EE,201b5ca0,extended,3c014148", /* 3c0141c8 */
					"patch=1,EE,201FC230,extended,2C42003c", /* 2C42001E */
					"patch=1,EE,E0040001,extended,002C3FA8",
					"patch=1,EE,2022088C,extended,8F82C960",
					"patch=1,EE,201E6028,extended,0C079FB0",
					"patch=1,EE,201b5ca0,extended,3c0141c8",
					"patch=1,EE,201FC230,extended,2C42001E",
					"patch=1,EE,E0010001,extended,002ECEF4",
					"patch=1,EE,2022088C,extended,8F82C960"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Project Zero 2: Crimson Butterfly (PAL)]: 50/60fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Psi-Ops: The Mindgate Conspiracy (PAL-M) [CRC: 5E7EB5E2] */
			else if (!strcmp(serial, "SLES-52702"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50/60fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,0017ACD8,word,00000000" /* 1640FFE5 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Psi-Ops: The Mindgate Conspiracy (PAL)]: 50/60fps patch applied.\n");
			}
			/* Rayman Revolution (PAL-M5) [CRC: 55EDA5A0] */
			else if (!strcmp(serial, "SLES-50044"))
			{
				/* Patch courtesy: ElHecht & ICUP321 */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,001011FC,word,24030000" /* 24030001 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Rayman Revolution (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Reign of Fire (PAL) [CRC: 79464D5E] */
			else if (!strcmp(serial, "SLES-50873"))
			{
				/* Patch courtesy: Gabominated */
				/* 50fps uncapped */
				static const char *const patches[] = {
					"patch=1,EE,00265C70,word,00000001", /* 00000002 */
					"patch=1,EE,00140A50,word,24020032"  /* 24020019 native global speed */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Reign of Fire (PAL)]: 50fps patch applied.\n");
			}
			/* Rune - Viking Warlord (PAL) [CRC: 52638022] */
			else if (!strcmp(serial, "SLES-50335"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					"patch=1,EE,001307AC,extended,28420001" /* 28420002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Rune: Viking Warlord (PAL)]: 50fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Scarface - The World is Yours (NTSC-U) [CRC: 41F4A178] */
			else if (!strcmp(serial, "SLES-54182"))
			{
				/* 50fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					"patch=1,EE,00DAFCBC,word,00000000" /* 00000001 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Scarface: The World Is Yours (PAL)]: 50fps patch applied (needs 180%% EE cyclerate).\n");
			}
			/* Sitting Ducks (PAL-M5) [CRC: 6B8D216E] */
			else if (!strcmp(serial, "SLES-52116"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. */
				static const char *const patches[] = {
					"patch=1,EE,0067DA58,extended,00000032",
					"patch=1,EE,E0010001,extended,0046786C",
					"patch=1,EE,0067DA58,extended,00000019"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sitting Ducks (PAL)]: 50fps patch applied.\n");
			}
			/* Smuggler's Run (PAL-M5) [CRC: 95416482] */
			else if (!strcmp(serial, "SLES-50061"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,002C6DA4,word,00000001" /* 00000002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Smuggler's Run (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Spawn - Armageddon (PAL) [CRC: 8C9BF4F9] */
			else if (!strcmp(serial, "SLES-52326"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=0,EE,00227CB0,word,24020001" /* 24020002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Spawn: Armageddon (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Splinter Cell - Pandora Tomorrow (PAL) [CRC: 80FAC91D] */
			else if (!strcmp(serial, "SLES-52149"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,0018D7C8,word,24030001" /* 24030002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Splinter Cell: Pandora Tomorrow (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Unreal Tournament (PAL-M5) [CRC: 4A805DF1] */
			else if (!strcmp(serial, "SLES-50074"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 180%. */
				static const char *const patches[] = {
					"patch=1,EE,0012D394,word,28420001" /* 28420002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Unreal Tournament (PAL)]: 50fps patch applied (needs 180%% EE cyclerate).\n");
			}
		}
		else if (!strncmp("SCES-", serial, strlen("SCES-")))
		{
			/* Dog's Life, The (PAL-M) [CRC: 531061F2] */
			if (!strcmp(serial, "SCES-51248"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,00791350,word,3CA3D70A"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dog's Life, The (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Drakan - The Ancients Gate (PAL-M) [CRC: 04F9D87F] */
			else if (!strcmp(serial, "SCES-50006"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,001D7950,extended,28420002",
					"patch=1,EE,E0010001,extended,004DBA28",
					"patch=1,EE,001D7950,extended,28420004"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Drakan: The Ancients Gate (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Final Fantasy X (PAL) */
			else if (!strcmp(serial, "SCES-50494"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,0011B940,extended,24040002",
					"patch=1,EE,2057C7D8,extended,3D4CCCCD", /* Move NPC and Tidus */
					"patch=1,EE,2058D448,extended,40400000", /* Text */
					"patch=1,EE,E0030001,extended,005808B4",
					"patch=1,EE,0011B940,extended,24040001",
					"patch=1,EE,2057C7D8,extended,3CCCCCCD",
					"patch=1,EE,2058D448,extended,3FC00000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Final Fantasy X (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
			/* Getaway, The (PAL-M) [CRC: 458485EF] */
			else if (!strcmp(serial, "SCES-51159"))
			{
				/* Patch courtesy: PeterDelta */
				/* 50fps uncapped. Need EE Overclock at 130%. */
				static const char *const patches[] = {
					"patch=1,EE,001F0EB8,word,24020001" /* 24020002 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Getaway, The (PAL)]: 50fps patch applied (needs 130%% EE cyclerate).\n");
			}
		}
	}
	
	if (hint_widescreen > 0)
	{
		if (!strncmp("SLUS-", serial, strlen("SLUS-")))
		{
			/* 24 The Game (NTSC-U) */
			if (!strcmp(serial, "SLUS-21268"))
			{
				/* Force turn on the native widescreen */
				static const char *const patches[] = {
					"patch=1,EE,205FBD2C,word,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [24: The Game (NTSC-U)]: Force native widescreen mode patch applied.\n");
			}
			/* Ace Combat Zero: The Belkan War (NTSC-U) */
			else if (!strcmp(serial, "SLUS-21346"))
			{
				switch (hint_widescreen)
				{
					case 3: /* 21:9 */
						{
							/* Patch courtesy: pgert */
							static const char *const patches[] = {
								"patch=1,EE,003FA350,word,440C0000",
								"patch=1,EE,003FA354,word,444DA000"
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Ace Combat Zero: The Belkan War (NTSC-U)]: 21:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default: /* 16:9 */
						{
							/* Patch courtesy: nemesis2000 */
							static const char *const patches[] = {
								"patch=1,EE,003FA350,word,43D638F3",
								"patch=1,EE,003FA354,word,43EB7385"
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Ace Combat Zero: The Belkan War (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Aggressive Inline (NTSC-U) [CRC: 67DB3ED8] */
			else if (!strcmp(serial, "SLUS-20327"))
			{
				/* Patch courtesy: BloodRaynare, ElHecht */
				static const char *const patches[] = {
					"patch=1,EE,00101130,word,3c013f40", /* 3c013f80 hor fov */
					"patch=1,EE,00272894,word,3c093f40", /* 00000000 renderfix */
					"patch=1,EE,00101714,word,0809ca25", /* 46000843 */
					"patch=1,EE,00101718,word,00000000", /* e62004e8 */
					"patch=1,EE,0027289c,word,4489f000", /* 00000000 */
					"patch=1,EE,002728a0,word,461e0843", /* 00000000 */
					"patch=1,EE,002728a4,word,46000843", /* 00000000 */
					"patch=1,EE,002728a8,word,e62004e8", /* 00000000 */
					"patch=1,EE,002728ac,word,080405c6" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Aggressive Inline (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Alias (NTSC-U) [CRC: E3ADDC73] */
			else if (!strcmp(serial, "SLUS-20673"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00248178,word,3c013ec0", /* 3c013f00 hor fov */
					"patch=1,EE,001f3c30,word,3c013f40" /* 3c013f80 renderfix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Alias (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Ape Escape 2 (NTSC-U) [CRC: BDD9F5E1] */
			else if (!strcmp(serial, "SLUS-20685"))
			{
				/* Patch courtesy: CRASHARKI */
				static const char *const patches[] = {
					"patch=1,EE,9039f070,extended,0c0e7bc4",
					/* Default to builtin 16:9 widescreen mode from the start. */
					"patch=1,EE,203e06a4,extended,00000001" /* 0 Widescreen */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ape Escape 2 (NTSC-U)]: Force native widescreen mode patch applied.\n");
			}
			/* Armored Core 2 (NTSC-U) [CRC: F3F906DE] */
			else if (!strcmp(serial, "SLUS-20014")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,202B5880,extended,43f00000",
					"patch=1,EE,001b4508,extended,3c013f40",
					"patch=1,EE,001c54e4,extended,3c013f40",
					"patch=1,EE,001c5614,extended,3c013f40"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Armored Core 2 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Armored Core 3 (NTSC-U) [CRC: FDB4D261] */
			else if (!strcmp(serial, "SLUS-20435")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,00199924,word,3c013f22",
					"patch=1,EE,002c4be4,word,3c013f40",
					"patch=1,EE,204279EC,word,43f00000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Armored Core 3 (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Auto Modellista (NTSC-U) [CRC: 6D76177B] */
			else if (!strcmp(serial, "SLUS-20642"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0039b80c,word,3fe38e38",
					"patch=1,EE,0022be20,word,3c023fe3",
					"patch=1,EE,0022be28,word,34428e38"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Auto Modellista (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Beat Down: Fists of Vengeance (NTSC-U) [CRC: C9F6EF9A] */
			else if (!strcmp(serial, "SLUS-21150"))
			{
				static const char *const patches[] = {
					/* 16:9 (42081546 43080046 00000000 00008244 00000000(*) 030b0046 00000000 00000000(*) da95040c 00000000(*)) */
					"patch=1,EE,00375388,word,3c013f40", /* 00000000 hor fov */
					"patch=1,EE,00375394,word,4481f000", /* 00000000 */
					"patch=1,EE,0037539c,word,461ea503"  /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Beat Down: Fists of Vengeance (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Blood Omen 2: The Legacy of Kain Series (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20024"))
			{
				static const char *const patches[] = {
					/* Widescreen */
					"patch=1,EE,00312b08,word,3C013FE3",
					"patch=1,EE,00312b0c,word,34218E38",
					/* Black Borders Fix */
					"patch=1,EE,002d443c,word,00000000",
					/* FMV Fix */
					"patch=1,EE,002eb280,word,240575e0", /* y-position */
					"patch=1,EE,002eb298,word,240a1440"  /* y-scaling */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Blood Omen 2: The Legacy of Kain (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Bloody Roar 3 (NTSC-U) [CRC: AA4E5A35] */
			else if (!strcmp(serial, "SLUS-20212"))
			{
				static const char *const patches[] = {
					"patch=1,EE,201f4454,extended,3F400000" /* 16:9 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Bloody Roar 3 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Bloody Roar 4 (NTSC-U) [CRC: C9F6F222] */
			else if (!strcmp(serial, "SLUS-20795")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,00115768,word,3c013fe3"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Bloody Roar 4 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* BMX XXX (SLUS-20415) [CRC: 2999BCF9] */
			else if (!strcmp(serial, "SLUS-20415"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00101074,word,3c013f40", /* 3c013f80 hor fov */
					"patch=1,EE,00299544,word,3c093f40", /* 00000000 renderfix */
					"patch=1,EE,001014a4,word,080a6551", /* 46000843 */
					"patch=1,EE,001014a8,word,00000000", /* e62404e8 */
					"patch=1,EE,0029954c,word,4489f000", /* 00000000 */
					"patch=1,EE,00299550,word,461e0843", /* 00000000 */
					"patch=1,EE,00299554,word,46000843", /* 00000000 */
					"patch=1,EE,00299558,word,e62004e8", /* 00000000 */
					"patch=1,EE,0029955c,word,0804052a" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [BMX XXX (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Breath of Fire: Dragon Quarter (NTSC-U) [CRC: 588CC41B] */
			else if (!strcmp(serial, "SLUS-20499"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0012dd1c,word,3c024307", /* 16:9 hor val (orig: 3C024333) */
					"patch=1,EE,0012de68,word,3c034074" /* render fix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Breath of Fire: Dragon Quarter (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Britney's Dance Beat (NTSC-U) [CRC: 3EAD47FE] */
			else if (!strcmp(serial, "SLUS-20402"))
			{
				static const char *const patches[] = {
					"patch=1,EE,203467B4,word,3F400000" /* 16:9 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Britney's Dance Beat (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Bouncer, The (NTSC-U) [CRC: FEE23E8F] */
			else if (!strcmp(serial, "SLUS-20069"))
			{
				static const char *const patches[] = {
					"patch=1,EE,203923BC,extended,3F400000" /* 16:9 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Bouncer, The (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Bujingai - The Forsaken City (NTSC-U) [CRC: 521D40D2] */
			else if (!strcmp(serial, "SLUS-20895")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,204B4580,extended,3F400000", /* 3F800000  hor+ */
					"patch=1,EE,2035C5F8,extended,3F990000"  /* 3F800000  orbs fix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Bujingai - The Forsaken City (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Burnout Revenge (NTSC-U) [CRC: D224D348] */
			else if (!strcmp(serial, "SLUS-21242"))
			{
				static const char *const patches[] = {
					/* Force native widescreen mode */
					"patch=0,EE,004693D4,extended,00000001",
					"patch=0,EE,204693D8,extended,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-U)]: Force native widescreen mode patch applied.\n");
			}
			/* Castlevania: Curse of Darkness (NTSC-U) [CRC: 3A446111] */
			else if (!strcmp(serial, "SLUS-21168"))
			{
				static const char *const patches[] = {
					/* gameplay */
					"patch=1,EE,00751384,word,3c013f40", /* hor value */
					"patch=1,EE,00751388,word,44810000",
					"patch=1,EE,00751390,word,4600c602",
					"patch=1,EE,009cfaf0,word,c4560000", /* item pos-x fix */
					"patch=1,EE,00443eb8,word,00000000", /* FMV fix */
					"patch=1,EE,00775398,word,24056e40",
					"patch=1,EE,007753a4,word,24072380",
					/* camera x-axis invert */
					"patch=1,EE,004446cc,word,46000007",
					"patch=1,EE,004446d0,word,03e00008",
					"patch=1,EE,004446d4,word,27bd0010",
					"patch=1,EE,0044b2ec,word,14400005",
					"patch=1,EE,0044b2f4,word,c6000000",
					"patch=1,EE,0044b2fC,word,e6000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Castlevania: Curse of Darkness (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Chaos Legion (NTSC-U) [CRC: F3B0734E] */
			else if (!strcmp(serial, "SLUS-20695")) /* 16:9 */
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,00242f4c,word,3c013f40",
					"patch=1,EE,00242f5c,word,4481d800",
					"patch=1,EE,00242f60,word,460fdec2",
					"patch=1,EE,00243064,word,3c0243ab",
					"patch=1,EE,00228064,word,3c023fab",
					/* FMVs fix */
					"patch=1,EE,00325684,word,3c026fb0",
					"patch=1,EE,00325594,word,241e77d0"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Chaos Legion (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Cold Winter (NTSC-U) [CRC: D6D704BB] */
			else if (!strcmp(serial, "SLUS-20845"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,003c4cf4,word,3c013f40",
					"patch=1,EE,003c4cfc,word,44810800",
					"patch=1,EE,003c4d00,word,03e00008",
					"patch=1,EE,003c4d04,word,4601a083",
					/* weapon */
					"patch=1,EE,00310f4c,word,c6740330",
					"patch=1,EE,00310f54,word,c7a30048",
					"patch=1,EE,00310f60,word,e7a20050",
					"patch=1,EE,00310f64,word,c6620330",
					/* gameplay */
					"patch=1,EE,001dcc34,word,8E440058",
					"patch=1,EE,001dcc40,word,e7a20040"

				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Cold Winter (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Crash Bandicoot Wrath of Cortex (NTSC-U) [CRC: 103B5706/5188ABCA] */
			else if (!strcmp(serial, "SLUS-20238"))
			{
				/* 5188ABCA - v1.0 */
				/* 103B5706 - v1.1 */
				if (game_crc == 0x5188ABCA) /* v1.0 */
				{
					/* Patch courtesy: flameofrecca/PsxFan107 */
					static const char *const patches[] = {
						"patch=1,EE,00113500,word,3c013f11", /* vertical fov */
						"patch=1,EE,0011207c,word,3c013f2a", /* zoom value */
						"patch=1,EE,001124c8,word,3c013f2a" /* render value */
					};
					LOAD_PATCHES(patches);
				}
				else if (game_crc == 0x103B5706) /* v1.1 */
				{
					/* Patch courtesy: flameofrecca/PsxFan107 */
					static const char *const patches[] = {
						"patch=1,EE,001138B8,extended,3c013f11", /* vertical fov */
						"patch=1,EE,001127A0,extended,3c013f2a", /* zoom value */
						"patch=1,EE,0011287C,extended,3c013f2a" /* renderfix value */
					};
					LOAD_PATCHES(patches);
				}
				log_cb(RETRO_LOG_INFO, "[PATCH] [Crash Bandicoot Wrath of Cortex (NTSC-U)]: 16:9 (Vert-) Widescreen patch applied.\n");
			}
			/* Crimson Tears (NTSC-U) [CRC: D31904C2] */
			else if (!strcmp(serial, "SLUS-20948"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,001bcb98,word,3c02bfe3", /* 3c02bfaa hor FOV */
					"patch=1,EE,001bcba0,word,34438e39" /* 3443aaab hor FOV */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Crimson Tears (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Devil Kings (NTSC-U) [CRC: B304172F] */
			else if (!strcmp(serial, "SLUS-21297"))
			{
				static const char *const patches[] = {
					"patch=1,EE,9028f058,extended,0c0a3bbe",
					"patch=1,EE,20130afc,extended,3c013f19",
					"patch=1,EE,20130b00,extended,3421999a",
					"patch=1,EE,201ba360,extended,3c013f19",
					"patch=1,EE,201ba364,extended,3421999a"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Devil Kings (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Devil May Cry (NTSC-U) [CRC: ] */
			else if (!strcmp(serial, "SLUS-20216"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0014e478,word,3c023f19",
					"patch=1,EE,0014e47c,word,3448999a",
					"patch=1,EE,0024fc14,word,0c040b90",
					"patch=1,EE,0024fc18,word,00000000",
					"patch=1,EE,0024fc1c,word,4615a800",
					"patch=1,EE,0024fc20,word,3c0342a0",
					"patch=1,EE,0024fc24,word,3c010075",
					"patch=1,EE,0024fc28,word,8c242ec0",
					"patch=1,EE,0024fc2c,word,44830800",
					"patch=1,EE,0024fc30,word,3c024220",
					"patch=1,EE,0024fc34,word,46000803",
					"patch=1,EE,0024fc38,word,3c033f00", /* vert renderfix */
					"patch=1,EE,0024fc3c,word,44822000",
					"patch=1,EE,0024fc40,word,44831800",
					"patch=1,EE,0024fc44,word,3c033f30", /* hor renderfix */
					"patch=1,EE,0024fc48,word,46030042",
					"patch=1,EE,0024fc4c,word,e4810024",
					"patch=1,EE,0024fc50,word,44830800",
					"patch=1,EE,0024fc54,word,46010042",
					"patch=1,EE,0024fc58,word,4604a0c0",
					"patch=1,EE,0024fc5c,word,8c222ec0",
					"patch=1,EE,0024fc60,word,4604a081",
					"patch=1,EE,0024fc64,word,e4810010",
					"patch=1,EE,0024fc68,word,3c02c000",
					"patch=1,EE,0024fc6c,word,44820800",
					"patch=1,EE,0024fc70,word,3c033f80",
					"patch=1,EE,0024fc74,word,4604a002",
					"patch=1,EE,0024fc78,word,8c222ec0",
					"patch=1,EE,0024fc7c,word,46000802",
					"patch=1,EE,0024fc80,word,46021843",
					"patch=1,EE,0024fc84,word,e4410038",
					"patch=1,EE,0024fc88,word,8c222ec0",
					"patch=1,EE,0024fc8c,word,46020003",
					"patch=1,EE,0024fc90,word,e4400048"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Devil May Cry (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Devil May Cry 3 (NTSC-U) [CRC: 0BED0AF9] */
			else if (!strcmp(serial, "SLUS-20964"))
			{
				static const char *const patches[] = {
					"patch=1,EE,21CB0590,extended,3F400000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Devil May Cry 3 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Devil May Cry 3: Special Edition (NTSC-U) [CRC: 25FC361B] */
			else if (!strcmp(serial, "SLUS-21361"))
			{
				/* Patch courtesy: No.47 */
				static const char *const patches[] = {
					"patch=1,EE,21D0DEA0,word,3F400000", /* horizontal fov */
					"patch=1,EE,206A2870,word,3F400000", /* hud fix, pause screen fix */
					/* FMV's fix */
					"patch=1,EE,0023279c,word,240301aa", /* intro / demo */
					"patch=1,EE,002e52ec,word,240801aa" /* cutscenes */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Devil May Cry 3: Special Edition (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Disaster Report (NTSC-U) [CRC: 7D7D4D9D] */
			else if (!strcmp(serial, "SLUS-20561"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0013d778,word,3c023f40",
					"patch=1,EE,0013dfd4,word,3c023f40",
					"patch=1,EE,0013e158,word,3c023f40",
					"patch=1,EE,0025be20,word,43e00000",
					"patch=1,EE,0025be30,word,43e00000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Disaster Report (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* DOA2: Hardcore (NTSC-U) [CRC: 23AF6876] */
			else if (!strcmp(serial, "SLUS-20071"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0021b63c,word,3c014534" /* 16:9 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [DOA2: Hardcore (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Dino Stalker (NTSC-U) [CRC: 3FBF0EA6] */
			else if (!strcmp(serial, "SLUS-20485"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,202AF944,extended,3f199999", /* 16:9 */
					"patch=1,EE,202AF984,extended,3f199999",
					"patch=1,EE,202AF9c4,extended,3f199999",
					"patch=1,EE,0012d224,word,3c0143d6",
					"patch=1,EE,00117670,word,3c0143d6"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dino Stalker (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Dirge of Cerberus - Final Fantasy VII (NTSC-U) [CRC: 44A5FA15] */
			else if (!strcmp(serial, "SLUS-21419"))
			{
				/* Patch courtesy: Gabominated */
				static const char *const patches[] = {
					"patch=1,EE,004FB248,word,3FC962FC", /* 3F970A3D y-fov */
					"patch=1,EE,004FB250,word,42b40000", /* 42930000 zoom */
					"patch=1,EE,0040c5d4,word,3c013fab", /* 3c013f80 render y-fix */
					"patch=1,EE,0040c5f4,word,3c013fab"  /* 3c013f80 render y-fix */
				};
				LOAD_PATCHES(patches);
				switch (hint_widescreen)
				{
					case 3: /* 21:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,004FB248,word,3FE00000" /* 3F970A3D y-fov */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Dirge of Cerberus: Final Fantasy VII (NTSC-U)]: 21:9 (Vert-) Widescreen patch applied.\n");
						}
						break;
					default: /* 16:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,004FB248,word,3FC962FC" /* 3F970A3D y-fov */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Dirge of Cerberus: Final Fantasy VII (NTSC-U)]: 16:9 (Vert-) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Driving Emotion Type-S (NTSC-U) [CRC: 1FCC0CFB] */
			else if (!strcmp(serial, "SLUS-20113")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,0045B2A8,word,3F400000" /* 3F800000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Driving Emotion Type-S (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Dynasty Warriors 2 (NTSC-U) [CRC: 5B665C0B] */
			else if (!strcmp(serial, "SLUS-20079"))
			{
				static const char *const patches[] = {
					"patch=1,EE,9032b488,word,0c0cacc8",
					"patch=1,EE,202fa696,word,3faaaaab",
					"patch=1,EE,202fb4ec,word,3c013fab",
					"patch=1,EE,203320cc,word,3faaaaab",
					"patch=1,EE,203321e8,word,3faaaaab",
					"patch=1,EE,203321f4,word,3faaaaab",
					"patch=1,EE,20332208,word,3faaaaab",
					"patch=1,EE,20332214,word,3faaaaab",
					"patch=1,EE,2033226c,word,3faaaaab",
					"patch=1,EE,2033236c,word,3faaaaab",
					"patch=1,EE,2033b18c,word,3faaaaab",
					"patch=1,EE,2033b190,word,3faaaaab",
					"patch=1,EE,2033b194,word,3faaaaab",
					/* Render fix (fix by Arapapa) */
					"patch=1,EE,20253d24,word,3c013f40"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 2 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Dynasty Warriors 4 (NTSC-U) [CRC: 6C89132B] */
			else if (!strcmp(serial, "SLUS-20653"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,00136f30,word,3c0243d6",
					"patch=1,EE,00183dc0,word,3c023f2b"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Dynasty Warriors 4: Empires (NTSC-U) [CRC: BD3DBCF9] */
			else if (!strcmp(serial, "SLUS-20938"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,00139b54,word,3c0243d6",
					"patch=1,EE,00188da0,word,3c023f2b"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4: Empires (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Dynasty Warriors 4: Xtreme Legends (NTSC-U) [CRC: 96C20D6F] */
			else if (!strcmp(serial, "SLUS-20812"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,001396c0,word,3c0243d6",
					"patch=1,EE,0018e0f0,word,3c023f2b"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 4: Xtreme Legends (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Dynasty Warriors 5 (NTSC-U) [CRC: 6677B437] */
			else if (!strcmp(serial, "SLUS-21153"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,00146c48,word,3c0243d6",
					"patch=1,EE,00181cec,word,3c023f2b"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 5 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Dynasty Warriors 5: Xtreme Legends (NTSC-U) [CRC: A719D130] */
			else if (!strcmp(serial, "SLUS-21299"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,00146d7c,word,3c0243d6",
					"patch=1,EE,0019814c,word,3c023f2b"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 5: Xtreme Legends (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Dynasty Warriors 6 (NTSC-U) [CRC: 047571F1] */
			else if (!strcmp(serial, "SLUS-21774"))
			{
				static const char *const patches[] = {
					"patch=1,EE,901227d0,extended,0c04899c",
					"patch=1,EE,20147178,extended,3c0243b4",
					"patch=1,EE,20147238,extended,3c0243f0",
					"patch=1,EE,201556a4,extended,3c023f15"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dynasty Warriors 6 (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Echo Night - Beyond (NTSC) [CRC: 2DE16D21] */
			else if (!strcmp(serial, "SLUS-20928"))
			{
				static const char *const patches[] = {
					/* Force turn on the native Widescreen
					 * 01 00 00 00 00 00 00 3F */
					"patch=1,EE,202ADC01,byte,00000001",
					/* 703f033c 003f023c d7a36334 */
					"patch=1,EE,00143d14,word,3c023f1f" /* 3c023f00 Zoom */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Echo Night: Beyond (NTSC-U)]: Force native widescreen mode patch applied.\n");
			}
			/* Enter The Matrix (v1.01) (NTSC-U) [CRC: 67EA565CB] */
			else if (!strcmp(serial, "SLUS-20454"))
			{
				static const char *const patches[] = {
					"patch=1,EE,001CF170,word,a2740bcc"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Enter The Matrix (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Enthusia - Professional Racing (NTSC-U) [CRC: 81D233DC] */
			else if (!strcmp(serial, "SLUS-20967")) 
			{
				/* Patch courtesy: No.47 */
				static const char *const patches[] = {
					"patch=1,EE,20383708,word,3F400000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Enthusia - Professional Racing (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Eternal Ring (NTSC-U) [CRC: C79B9F4F7] */
			else if (!strcmp(serial, "SLUS-20015"))
			{
				static const char *const patches[] = {
					/* X-Fov - ELF hack
					 * 803f013c 00a88144 0045013c */
					"patch=1,EE,00100fcc,word,3c013f40" /* 3c013f80 */
					/* Memory hack
					 * patch=1,EE,201FF100,word,43c00000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Eternal Ring (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Evergrace (NTSC-U) [CRC: 758F0EE6] */
			else if (!strcmp(serial, "SLUS-20016"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00101128,extended,3c013f40",
					"patch=1,EE,001011c8,extended,3c013f40"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Evergrace (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Fatal Frame II: Crimson Butterfly (NTSC-U) [CRC: 9A51B627] */
			else if (!strcmp(serial, "SLUS-20766"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0033760c,word,3f400000", /* aspect */
					/* FMVs fix */
					"patch=1,EE,001e598c,word,3c013f40", /* width (1.0f = 640) */
					"patch=1,EE,001e5990,word,0c0795e0",
					"patch=1,EE,001e5994,word,44811800",
					"patch=1,EE,001e5998,word,8f84be5c",
					"patch=1,EE,001e599c,word,0c079efe",
					"patch=1,EE,001e59a0,word,0040802d",
					"patch=1,EE,001e59a4,word,0200102d",
					"patch=1,EE,001e59a8,word,dfbf0008",
					"patch=1,EE,001e59ac,word,27bd0010",
					"patch=1,EE,001e59b0,word,03e00008",
					"patch=1,EE,001e59b4,word,dfb00000",

					"patch=1,EE,001e5834,word,01c02820",
					"patch=1,EE,001e5838,word,e4830030",

					/* x-pos = (640-640*width)/(2*width) Hex */
					"patch=1,EE,0033b228,word,0000006a",
					/* Lens Flares fix */
					"patch=1,EE,0014f72c,word,3c013f40",
					"patch=1,EE,0014f79c,word,44810800",
					"patch=1,EE,0014f7a0,word,46016302",
					"patch=1,EE,0013a19c,word,0c053dcb",
					"patch=1,EE,0013a224,word,0c053dcb",
					"patch=1,EE,0013a28c,word,0c053dcb",
					"patch=1,EE,0013a304,word,0c053dcb"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Fatal Frame II: Crimson Butterfly (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Gauntlet: Seven Sorrows (NTSC-U) [CRC: A8C4C0A9] */
			else if (!strcmp(serial, "SLUS-21077"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,00446200,word,24020002" /* built in widescreen */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gauntlet: Seven Sorrows (NTSC-U)]: Force native widescreen mode patch applied.\n");
			}
			/* God Hand (NTSC-U) [CRC: 6FB69282] */
			else if (!strcmp(serial, "SLUS-21503"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0030b8f0,word,3c013f9f",
					"patch=1,EE,0030b8f4,word,342149f1"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [God Hand (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [God Hand (NTSC-U)]: TODO/FIXME - doesn't seem correct.\n");
			}
			/* Gradius V (NTSC-U) [CRC: CDA95971] */
			else if (!strcmp(serial, "SLUS-20712"))
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* X-Fov
					 * 803f033c 1855040c 06a30046 */
					"patch=1,EE,001d9228,word,3c033f72", /* 3c033f80 */
					/* Render fix
					 * 803f033c 6400a2af 0070023c */
					"patch=1,EE,002e0638,word,3c033fb0", /* 3c033f80 */
					"patch=1,EE,002e0948,word,3c033fb0", /* 3c033f80 ?? */
					"patch=1,EE,002e1038,word,3c033fb0", /* 3c033f80 */
					"patch=1,EE,002e1178,word,3c033fb0" /* 3c033f80 ?? */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gradius V (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Grand Theft Auto: Vice City (NTSC-U) [CRC: 20B19E49] */
			else if (!strcmp(serial, "SLUS-20552"))
			{
				/* Same revision guard as the 60fps entry above: these
				 * addresses are only valid for the CRC 20B19E49 ELF. */
				if (game_crc == 0x20B19E49)
				{
					static const char *const patches[] = {
						/* 16:9 */
						"patch=1,EE,001325BC,word,3C013F9D",
						"patch=1,EE,001325C0,word,44810000",
						"patch=1,EE,001325C4,word,46006302",
						"patch=1,EE,001325C8,word,03E00008",
						"patch=1,EE,001325CC,word,E78C86F8",

						"patch=1,EE,002434EC,word,0C04C96F",
						"patch=1,EE,0026FE1C,word,0C04C972",
						"patch=1,EE,002703F4,word,0C04C972"
					};
					LOAD_PATCHES(patches);
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: Vice City (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
				else if (game_crc == 0x248E6126)
				{
					/* v4.00 pressing. Same transform against this ELF's layout:
					 * the aspect setter is jr ra; swc1 f12,-0x7908(gp) at
					 * 1324C0/C4 with three words of padding before it; the scale
					 * sequence goes in that padding and the caller at 2485DC is
					 * redirected to it. The other two callers (27579C, 275D6C)
					 * keep the plain entry, written out explicitly like the
					 * 20B19E49 entry above. All nine words verified against the
					 * ELF and a running image. */
					static const char *const patches_v400[] = {
						/* 16:9 */
						"patch=1,EE,001324B4,word,3C013F9D",
						"patch=1,EE,001324B8,word,44810000",
						"patch=1,EE,001324BC,word,46006302",
						"patch=1,EE,001324C0,word,03E00008",
						"patch=1,EE,001324C4,word,E78C86F8",

						"patch=1,EE,002485DC,word,0C04C92D",
						"patch=1,EE,0027579C,word,0C04C930",
						"patch=1,EE,00275D6C,word,0C04C930"
					};
					LOAD_PATCHES(patches_v400);
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: Vice City (NTSC-U) v4.00]: 16:9 Widescreen patch applied.\n");
				}
				else
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: Vice City (NTSC-U)]: 16:9 Widescreen patch skipped, no patch for this revision (CRC %08X).\n", game_crc);
			}
			/* Grand Theft Auto: San Andreas (NTSC-U) [CRC: 399A49CA] */
			else if (!strcmp(serial, "SLUS-20946"))
			{
				/* Same revision guard as the entries above: the aspect
				 * setter tail (jr ra; swc1 f12,-0x6570(gp)) sits at
				 * 1130C0/C4 in the 399A49CA ELF and both callers jal it;
				 * on any other pressing these words land elsewhere. */
				if (game_crc == 0x399A49CA)
				{
					/* Patch courtesy: nemesis2000, flameofrecca */
					static const char *const patches[] = {
						"patch=1,EE,001130BC,word,3C013F9D",
						"patch=1,EE,001130C0,word,44810000",
						"patch=1,EE,001130C4,word,46006302",
						"patch=1,EE,001130C8,word,03E00008",
						"patch=1,EE,001130CC,word,E78C9A90",
						"patch=1,EE,0021DF84,word,0C044C2F",
						"patch=1,EE,00242D54,word,0C044C32"
					};
					LOAD_PATCHES(patches);
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: San Andreas (NTSC-U)]: 16:9 Widescreen patch applied.\n");
				}
				else
					log_cb(RETRO_LOG_INFO, "[PATCH] [Grand Theft Auto: San Andreas (NTSC-U)]: 16:9 Widescreen patch skipped, no patch for this revision (CRC %08X).\n", game_crc);
			}
			/* Half-Life (NTSC-U) [CRC: A880AE9B] */
			else if (!strcmp(serial, "SLUS-20066")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,002aa91c,extended,3c013fe3",
					"patch=1,EE,002aa920,extended,34218e38",
					"patch=1,EE,002aa158,extended,3c013f2b"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Half-Life (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Haunting Ground (NTSC-U) [CRC: 901AAC09] */
			else if (!strcmp(serial, "SLUS-21075")) /* 16:9 */
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0010e31c,word,3c013f40",
					"patch=1,EE,0010e320,word,44810000",
					"patch=1,EE,0010e328,word,4600c602",
					"patch=1,EE,002ba3ec,word,34a98c00",
					"patch=1,EE,002ba3d4,word,34a67400"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Haunting Ground (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* King's Field: The Ancient City (NTSC-U) [CRC: 36E02E91] */
			else if (!strcmp(serial, "SLUS-20318")) /* 16:9 */
			{
				/* Patch courtesy: pelvicthrustman */
				static const char *const patches[] = {
					"patch=1,EE,0022d8e4,word,3c013f40", /* 00000000 */
					"patch=1,EE,0022d8e8,word,44810000", /* 00000000 */
					"patch=1,EE,0022d8f0,word,4600c602" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [King's Field: The Ancient City (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Kingdom Hearts 2 (NTSC-U) [CRC: DA0535FD] */
			else if (!strcmp(serial, "SLUS-21005")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,00106c70,word,3c013f4c", /* c480004c */
					"patch=1,EE,00106c88,word,3421cccc", /* 4600a7c6 */
					"patch=1,EE,00106c8c,word,4481f800", /* 00000000 */
					"patch=1,EE,00106c90,word,461fa503", /* 4600a503 */
					"patch=1,EE,00106cb4,word,3c1b3f40", /* 00000000 hor fov */
					"patch=1,EE,00106cc0,word,449bf000", /* 00000000 */
					"patch=1,EE,00106cd0,word,461effc2", /* 00000000 */
					"patch=1,EE,00106cd4,word,e61f004c", /* 00000000 */
					"patch=1,EE,20378104,word,43f90000", /* 43d00000 optional zoom for cutscenes (hides sudden pop-in) */
					"patch=1,EE,2037ae44,word,3F400000", /* 3F800000 font fix */
					"patch=1,EE,2037ae48,word,3F400000", /* 3F800000 */
					"patch=1,EE,2037ae4c,word,3F400000", /* 3F800000 */
					"patch=1,EE,001aae88,word,240a0190" /* lower subtitles */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Kingdom Hearts II (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Lara Croft Tomb Raider - Angel of Darkness (NTSC-U) [CRC: 3BAEBCC3] */
			else if (!strcmp(serial, "SLUS-20467"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00220bd0,word,24020002", /* force turn on native widescreen */
					"patch=1,EE,00205230,word,3c013fe2", /* 3c013fd5 */
					"patch=1,EE,00205234,word,3421fc93"  /* 3421c28f */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Lara Croft Tomb Raider - Angel of Darkness (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Le Mans 24 Hours (NTSC-U) [CRC: 67835861] */
			else if (!strcmp(serial, "SLUS-20207"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00323AF4,word,3C033FAB"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Le Mans 24 Hours (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* MDK2: Armageddon (NTSC-U) [CRC: F191AFBC] */
			else if (!strcmp(serial, "SLUS-20105"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0017B418,word,3C0140AB"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [MDK2: Armageddon (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Mega Man X7 (NTSC-U) [CRC: 3EDA6DE7] */
			else if (!strcmp(serial, "SLUS-20487"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0014d3e4,word,3c0244a8" /* 16:9 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Mega Man X7 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Metal Gear Solid 3: Subsistence (NTSC-U) (Disc 1) [CRC: ] */
			else if (!strcmp(serial, "SLUS-21359"))
			{
				/* Patch courtesy: No.47, SolidSnake11 */
				static const char *const patches[] = {
					"patch=1,EE,202050AC,word,3F400000", /* widescreen (16:9) */
					/* No Letterbox */
					"patch=1,EE,D025E6A7,extended,00100001",
					"patch=1,EE,2025E6A4,extended,00000000",
					"patch=1,EE,D0145990,extended,8C8B0000",
					"patch=1,EE,D01459C8,extended,240F8000",
					"patch=1,EE,D0131758,extended,8E0E0004"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Metal Gear Solid 3: Subsistence (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Metal Gear Solid 3: Subsistence (NTSC-U)]: No letterbox patch applied.\n");
			}
			/* Midnight Club - Street Racing (NTSC-U) */
			else if (!strcmp(serial, "SLUS-20063")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,004784C0,word,3F471C97",
					"patch=1,EE,004784D4,word,3F471C97",
					"patch=1,EE,004784FC,word,3F471C97"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Midnight Club: Street Racing (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Mortal Kombat: Shaolin Monks (NTSC-U) [CRC: 455DD546] */
			else if (!strcmp(serial, "SLUS-21087"))
			{
				static const char *const patches[] = {
					"patch=1,EE,004f4b38,word,3f400000", /* 16:9 */
					"patch=1,EE,00322f2c,word,3c013f80", /* black border fix */
					"patch=1,EE,00272210,word,24030256", /* FMV fix */
					"patch=1,EE,00272498,word,240B0256"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Mortal Kombat: Shaolin Monks (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Mystic Heroes (NTSC-U) [CRC: 19C243C1] */
			else if (!strcmp(serial, "SLUS-20521"))
			{
				static const char *const patches[] = {
					"patch=1,EE,20324690,extended,3F400000" /* 3F800000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Mystic Heroes (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Need For Speed Underground 2 (NTSC-U) [CRC: F5C7B45F] */
			else if (!strcmp(serial, "SLUS-21065"))
			{
				static const char *const patches[] = {
					"patch=0,EE,20276E20,extended,A2C2004C" 
					/* auto enable in widescreen, boot option by default */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Need For Speed Underground 2 (NTSC-U)]: Force native widescreen mode patch applied.\n");
			}
			/* Nightshade (NTSC-U) [CRC: 519E816B] */
			else if (!strcmp(serial, "SLUS-20810")) /* 16:9 */
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,00183124,word,3c023f24",
					"patch=1,EE,00183128,word,34428b44",
					"patch=1,EE,0018314c,word,3c023f40",
					"patch=1,EE,00183100,word,3c014280",
					"patch=1,EE,0018310c,word,44815800",
					"patch=1,EE,002e1b40,word,e48b0070"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Nightshade (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Okami (NTSC-U) [CRC: 21068223] */
			else if (!strcmp(serial, "SLUS-21115")) /* 16:9 */
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0015c33c,word,3c0143a8",
					"patch=1,EE,0033ec38,word,3c013f9f",
					"patch=1,EE,0033ec3c,word,4481a800",
					"patch=1,EE,0015c360,word,3c013f9f",
					"patch=1,EE,0015c364,word,44817000",
					"patch=1,EE,0015c3ac,word,00000000",
					"patch=1,EE,0015c43c,word,3c014500",
					"patch=1,EE,0033ec20,word,3c014500"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Okami (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Onimusha 2 (NTSC-U) [CRC: 5848889C] */
			else if (!strcmp(serial, "SLUS-20393")) /* 16:9 */
			{
				/* Patch courtesy: nemesis2000, LittleGiant */
				static const char *const patches[] = {
					/* gameplay */
					"patch=1,EE,2010285c,word,3c013f40",
					"patch=1,EE,20102860,word,44810000",
					"patch=1,EE,20102868,word,4600c602"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Onimusha 2 (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Onimusha 3: Demon Siege (NTSC-U) [CRC: 6BF11378] */
			else if (!strcmp(serial, "SLUS-20694")) /* 16:9 */
			{
				static const char *const patches[] = {
					/* val1 */
					"patch=1,EE,00151320,word,3c033f19",
					"patch=1,EE,00151328,word,34639999",
					/* val2 */
					"patch=1,EE,00151380,word,3c023f19",
					"patch=1,EE,00151384,word,34439999",
					/* val3 */
					"patch=1,EE,001514d8,word,3c033f19",
					"patch=1,EE,001514e0,word,34639999",
					/* renderfixes */
					"patch=1,EE,00151550,word,3c02c3d6",
					"patch=1,EE,00151440,word,3c024527"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Onimusha 3: Demon Siege (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Onimusha: Dawn of Dreams (NTSC-U) [CRC: FE44479E] */
			else if (!strcmp(serial, "SLUS-21180"))
			{
				static const char *const patches[] = {
					/* val1 */
					"patch=1,EE,0012F960,word,3c033f19",
					"patch=1,EE,0012F968,word,34649999",
					/* val2 */
					"patch=1,EE,0012fb38,word,3c033f19",
					"patch=1,EE,0012fb40,word,34639999",
					/* val3 */
					"patch=1,EE,0012f9c8,word,3c023f19",
					"patch=1,EE,0012f9cc,word,34439999",
					/* renderfixes */
					"patch=1,EE,0012Fbb0,word,3c02c3d6",
					"patch=1,EE,0012Faa0,word,3c024527"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Onimusha: Dawn of Dreams (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Project - Snowblind (NTSC-U) [CRC: 2BDA8ADB] */
			else if (!strcmp(serial, "SLUS-21037"))
			{
				/* Patch courtesy: Gabominated */
				static const char *const patches[] = {
					"patch=1,EE,0090E9F4,word,3Fe38e39", /* 3FAAAAAB - x-fov */
					"patch=1,EE,00B764F4,word,3Fe38e39"  /* 3FAAAAAB - cutscenes */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Project: Snowblind (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Psi-Ops: The Mindgate Conspiracy (NTSC-U) [CRC: 9C71B59E] */
			else if (!strcmp(serial, "SLUS-20688"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0046DC3C,word,241102AA",
					/* FMV's fix */
					"patch=1,EE,00469938,word,2411012A"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Psi-Ops: The Mindgate Conspiracy (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* R-Type Final (NTSC-U) [CRC: 85E994DD] */
			else if (!strcmp(serial, "SLUS-20780"))
			{
				/* Patch courtesy: sergx12 */
				static const char *const patches[] = {
					"patch=1,EE,0016edc0,word,3c033f40", /* menu */
					"patch=1,EE,0016fbac,word,3c043f40", /* hor */
					"patch=1,EE,0022ac90,word,43d60000", /* renderfix */
					"patch=1,EE,0022aca0,word,43d60000" /* renderfix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [R-Type Final (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Red Faction (NTSC-U) [CRC: FBF28175] */
			else if (!strcmp(serial, "SLUS-20073")) 
			{
				/* Patch courtesy: ElHecht */
				switch (hint_widescreen)
				{
					case 2: /* 16:10 */
						{
							static const char *const patches[] = {
								"patch=1,EE,002071c4,word,3c013f55", /* 00000000 hor fov */
								"patch=1,EE,002071cc,word,34215555", /* 00000000 hor fov */
								"patch=1,EE,002071d0,word,4481f000", /* 00000000 */
								"patch=1,EE,002072e0,word,461ea502", /* 00000000 */
								"patch=1,EE,002072e8,word,461ead43", /* 00000000 */
								"patch=1,EE,0023a444,word,3c024318", /* 3c024334 shadow fix */
								"patch=1,EE,0023a34c,word,461e0303" /* 44826000 shadow fix */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Red Faction (NTSC-U)]: 16:10 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default: /* 16:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,002071c4,word,3c013f40", /* 00000000 hor fov */
								"patch=1,EE,002071d0,word,4481f000", /* 00000000 */
								"patch=1,EE,002072e0,word,461ea502", /* 00000000 */
								"patch=1,EE,002072e8,word,461ead43", /* 00000000 */
								"patch=1,EE,0023a444,word,3c024309", /* 3c024334 shadow fix */
								"patch=1,EE,0023a34c,word,461e0303" /* 44826000 shadow fix */
								
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Red Faction (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Red Ninja: End of Honor (NTSC-U) [CRC: 6B0F338D] */
			else if (!strcmp(serial, "SLUS-20714"))
			{
				static const char *const patches[] = {
					"patch=1,EE,003695dc,word,3c013f40", /* 3c013f80 hor fov1 */
					"patch=1,EE,003695e0,word,44817000", /* 44816000 */
					"patch=1,EE,003695f0,word,460e7303", /* 46006386 */

					"patch=1,EE,003fdc40,word,3c013f40", /* 00000000 hor fov2 (background trees) */
					"patch=1,EE,003fdc48,word,4481f000", /* 00000000 */
					"patch=1,EE,003fdc98,word,4602f782", /* 00000000 */
					"patch=1,EE,003fdcac,word,461e0842", /* 46020002 */

					"patch=1,EE,0035dcc4,word,3c013f2b" /* 3c013f00 renderfix enemies */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Red Ninja: End of Honor (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Resident Evil - Dead Aim (NTSC-U) [CRC: FBB5290C] */
			else if (!strcmp(serial, "SLUS-20669"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00232c30,word,3c1943f0", /* 3c013f80 hor fov */
					"patch=1,EE,00232c34,word,46021003", /* 44810000 */
					"patch=1,EE,00232c64,word,ac99000c" /* e482000c */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Resident Evil: Dead Aim (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Return to Castle Wolfenstein: Operation Resurrection 
			 * (NTSC-U) [CRC: 5F4DB1DD] */
			else if (!strcmp(serial, "SLUS-20297"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0012f928,word,3c0142ab", /* ver FOV 1 */
					"patch=1,EE,0012f92c,word,3421e328",
					"patch=1,EE,001302dc,word,3c0142ab", /* ver FOV 2 */
					"patch=1,EE,001302e0,word,3421e328",
					"patch=1,EE,0012f6d8,word,3c0141e9", /* binoculars FOV */
					"patch=1,EE,0012f724,word,3c0142d5"  /* gameplay FOV */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Return to Castle Wolfenstein: Operation Resurrection (NTSC-U)]: 16:9 (Vert-) Widescreen patch applied.\n");
			}
			/* Ridge Racer V (NTSC-U) [CRC: 06AD9CA0] */
			else if (!strcmp(serial, "SLUS-20002"))
			{
				/* Patch courtesy: No.47 */
				static const char *const patches[] = {
					/* single player */
					"patch=1,EE,20332694,word,43C6C000", /* 44048000 - both fov, bumper camera */
					"patch=1,EE,203326B4,word,43C6C000", /* chase cam - 44048000 - both fov, chase camera (*) */
					"patch=1,EE,20332EC4,word,3F1D0364", /* 3EEB851F - vertical FOV */
					/* split screen */
					"patch=1,EE,20332690,word,43951000", /* 43C6C000 - both FOV */
					"patch=1,EE,20332ED0,word,3F1D0364", /* 3EEB851F - vert FOV, top */
					"patch=1,EE,20332ED4,word,3F1D0364", /* 3EEB851F - vert FOV, bottom */

					/* menu */
					"patch=1,EE,20332F80,word,3F199999", /* 3F4CCCCD - horizontal FOV */
					/* (*) improved chase cam that shows the whole car as 
					 * in other Ridge Racer games, instead of only the 
					 * upper half */
					/* (*) replace 43960000 by 43C6C000 for the 
					 * original chase cam */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ridge Racer V (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Rumble Roses (NTSC-U) [CRC: C1C91715] */
			else if (!strcmp(serial, "SLUS-20970"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00103fa4,word,3c013f40",
					"patch=1,EE,00103fa8,word,44810000",
					"patch=1,EE,00103fb0,word,4600c602",
					/* Font fix */
					"patch=1,EE,00305bbc,word,3c033f53", /* 3c033f8c */
					"patch=1,EE,00305bc0,word,34633333" /* 3463cccd */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Rumble Roses (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Second Sight (NTSC-U) [CRC: 16E3BE78] */
			else if (!strcmp(serial, "SLUS-21033")) /* 16:9 */
			{
				/* Patch courtesy: No.47 */
				static const char *const patches[] = {
					"patch=1,EE,20500C14,word,3FC71C71",
					"patch=1,EE,20500C94,word,3FC71C71"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Second Sight (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Shadow of Destiny (NTSC-U) [CRC: F14DFE0A] */
			else if (!strcmp(serial, "SLUS-20146")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,0020fdc8,word,3c014455",
					"patch=1,EE,0020fde0,word,3c013ac8"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Shadow of Destiny (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Shadow of Destiny (NTSC-U)]: NOTE: PCRTC Anti-blur needs to be enabled.\n");
			}
			/* Shadow of Rome (NTSC-U) [CRC: 57818AF6] */
			else if (!strcmp(serial, "SLUS-20902")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,00146bc8,word,3c023f1f",
					"patch=1,EE,00146bd0,word,344249f9",
					"patch=1,EE,00146de4,word,3c023f1f",
					"patch=1,EE,00146dec,word,344249f9",
					"patch=1,EE,001e746c,word,3c044328"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Shadow of Rome (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Shinobi (NTSC-U) [CRC: BFCC3E7E] */
			else if (!strcmp(serial, "SLUS-20459"))
			{
				/* Patch courtesy: paul_met */
				static const char *const patches[] = {
					"patch=1,EE,00234a50,word,3c023f40" /* 16:9 (orig: 3C023F80) */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Shinobi (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Sitting Ducks (NTSC-U) [CRC: 76A65B01] */
			else if (!strcmp(serial, "SLUS-20886")) /* 16:9 */
			{
				/* Patch courtesy: PeterDelta */
				static const char *const patches[] = {
					"patch=1,EE,00469CE8,word,3FE38E39"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sitting Ducks (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Soulcalibur III (NTSC-U) [CRC: 027C604C] */
			else if (!strcmp(serial, "SLUS-21216")) 
			{
				/* Patch courtesy: nemesis2000 */
				/* Correction of built-in widescreen mode */
				static const char *const patches[] = {
					"patch=1,EE,0012a118,word,3c013f40",
					"patch=1,EE,0012a11c,word,34210000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Soulcalibur III (NTSC-U)]: 16:9 Widescreen patch applied.\n");
				log_cb(RETRO_LOG_INFO, "[PATCH] [Soulcalibur III (NTSC-U)]: Correction of built-in widescreen mode applied.\n");
			}
			/* Street Fighter EX3 (NTSC-U) [CRC: 72B3802A] */
			else if (!strcmp(serial, "SLUS-20130"))
			{
				/* Patch courtesy: paul_met */
				static const char *const patches[] = {
					"patch=1,EE,002e34d4,word,3c013f40",
					"patch=1,EE,002e34d8,word,44810000",
					"patch=1,EE,002e34e0,word,4600c602"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Street Fighter EX3 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Stretch Panic (NTSC-U) [CRC: 854D5885] */
			else if (!strcmp(serial, "SLUS-20182")) 
			{
				switch (hint_widescreen)
				{
					case 2: /* 16:10 */
						{
							/* Patch courtesy: sergx12 */
							static const char *const patches[] = {
								/* 16:10 widescreen (ultra) */
								"patch=1,EE,2011FB4C,extended,08047EE0", /* 0441000C - j $0011fb80 - Jump over game logic for PADMAN.IRX debug error message to prevent conflicts */
								"patch=1,EE,2011FB08,extended,08047ECE", /* 0441000B - j $0011fb38 - Jump over game logic for SIO2MAN.IRX debug error message to prevent conflicts */
								"patch=1,EE,2012BFD8,extended,08047ECA", /* C60F017C - j $0011fb28 - Jump to injected MIPS instructions for cutscene/playing check */
								"patch=1,EE,2011FB28,extended,52600013", /* 0C043184 - beql s3, zero, $0011fb78 - Branch to the end of the widescreen logic if a cutscene is running (by checking if s3 register is 0) */
								"patch=1,EE,2011FB2C,extended,00000000", /* 70003628 - nop - Nop delay slot (to avoid a "branch XXXXXXXX in delay slot!" warning in PCSX2's log) */
								"patch=1,EE,2011FB30,extended,08047ED5", /* 0440FFF7 - j $0011fb54 - Jump to injected MIPS instructions for widescreen logic */
								"patch=1,EE,2011FB54,extended,3C013FAA", /* 3C02006A - lui at, $3faa - Set $f31 register to 1.333333373 #1 */
								"patch=1,EE,2011FB58,extended,3421AAAB", /* 0C046690 - ori at, at, $aaab - Set $f31 register to 1.333333373 #2 */
								"patch=1,EE,2011FB5C,extended,4481F800", /* 2444B2E0 - mtc1 at, $f31 - Set $f31 register to 1.333333373 #3 */
								"patch=1,EE,2011FB60,extended,461F6302", /* 3C02006A - mul.s $f12, $f12, $f31 - Multiply $f12 by $f31 and store the result in $f12 (positive X FOV) */
								"patch=1,EE,2011FB64,extended,46006347", /* 2444B2C0 - neg.s $f13, $f12 - Negate $f12 and store the result in $f13 (negative X FOV) */
								"patch=1,EE,2011FB68,extended,3C013F8E", /* 70002E28 - lui at, $3f8e - Set $f31 register to 1.111111164 #1 */
								"patch=1,EE,2011FB6C,extended,342138E4", /* 0C043184 - ori at, at, $38e4 - Set $f31 register to 1.111111164 #2 */
								"patch=1,EE,2011FB70,extended,4481F800", /* 70003628 - mtc1 at, $f31 - Set $f31 register to 1.111111164 #3 */
								"patch=1,EE,2011FB74,extended,461F7382", /* 0440FFF7 - mul.s $f14, $f14, $f31 - Multiply $f14 by $f31 and store the result in $f14 (negative Y FOV) */
								"patch=1,EE,2011FB78,extended,0804AFF8", /* 00000000 - j $0012bfe0 - Jump to 2 lines after the overwritten MIPS instruction */
								"patch=1,EE,2011FB7C,extended,460073C7" /* 00000000 - neg.s $f15, $f14 - Negate $f14 to make it positive and store the result in $f15 (positive Y FOV) */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Stretch Panic (NTSC-U)]: 16:10 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default: /* 16:9 */
						{
							/* Patch courtesy: sergx12 */
							static const char *const patches[] = {
								/* 16:9 widescreen */
								"patch=1,EE,2011FB4C,extended,08047EE0", /* 0441000C - j $0011fb80 - Jump over game logic for PADMAN.IRX debug error message to prevent conflicts */
								"patch=1,EE,2012BFD0,extended,08047ED5", /* C60D0174 - j $0011fb54 - Jump to injected MIPS instructions for widescreen logic */
								"patch=1,EE,2011FB54,extended,12600004", /* 3C02006A - beq s3, zero, $0011fb68 - Branch to the end of the widescreen logic if a cutscene is running (by checking if s3 register is 0) */
								"patch=1,EE,2011FB58,extended,3C013FAA", /* 0C046690 - lui at, $3faa - Set $f31 register to 1.333333373 #1 */
								"patch=1,EE,2011FB5C,extended,3421AAAB", /* 2444B2E0 - ori at, at, $aaab - Set $f31 register to 1.333333373 #2 */
								"patch=1,EE,2011FB60,extended,4481F800", /* 3C02006A - mtc1 at, $f31 - Set $f31 register to 1.333333373 #3 */
								"patch=1,EE,2011FB64,extended,461F6302", /* 2444B2C0 - mul.s $f12, $f12, $f31 - Multiply $f12 by $f31 and store the result in $f12 (positive X FOV) */
								"patch=1,EE,2011FB68,extended,0804AFF6", /* 70002E28 - j $0012bfd8 - Jump to 2 lines after the overwritten MIPS instruction */
								"patch=1,EE,2011FB6C,extended,46006347" /* 0C043184 - neg.s $f13, $f12 - Negate $f12 and store the result in $f13 (negative X FOV) */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Stretch Panic (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Tekken Tag Tournament (NTSC-U) [CRC: 67454C1E] */
			else if (!strcmp(serial, "SLUS-20001")) 
			{
				static const char *const patches[] = {
					"patch=0,EE,90402148,extended,0c1007f8",
					"patch=0,EE,2034b014,extended,3c013f40",
					"patch=0,EE,2034b018,extended,44810000",
					"patch=0,EE,2034b020,extended,4600c602"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken Tag Tournament (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Tekken 4 (NTSC-U) [CRC: 833FE0A4] */
			else if (!strcmp(serial, "SLUS-20328")) 
			{
				static const char *const patches[] = {
					/* EE patch */
					"patch=1,EE,002917E4,word,24020001",
					"patch=1,EE,002919E4,word,00000000",
					/* Gameplay */
					"patch=1,EE,00216EA0,word,3c013f40",
					/* Partial HUD fix */
					"patch=1,EE,001F6BF8,word,3C013F40",
					/* rfix 1 */
					"patch=1,EE,0018D408,word,3c0143d5",
					/* rfix 2 */
					"patch=1,EE,00200d84,word,3c013f40",
					"patch=1,EE,00200d88,word,44810000",
					"patch=1,EE,00200d94,word,46006303"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken 4 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Tekken 5 (NTSC-U) [CRC: 652050D2] */
			else if (!strcmp(serial, "SLUS-21059")) 
			{
				static const char *const patches[] = {
					"patch=1,EE,E0048870,extended,01FFEF20",
					"patch=1,EE,D03E453A,extended,0100",
					"patch=1,EE,0032B448,extended,40",
					"patch=1,EE,D03E453A,extended,0000",
					"patch=1,EE,0032B448,extended,80",

					"patch=1,EE,E0052C70,extended,01FFEF20",
					"patch=1,EE,2022E8FC,extended,000D6C3A",
					"patch=1,EE,2022E50C,extended,000E743A",
					"patch=1,EE,2021C9C0,extended,3C013F40",
					"patch=1,EE,2021C9CC,extended,4481F000",
					"patch=1,EE,2021C9D0,extended,461EBDC3"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken 5 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* TimeSplitters (NTSC-U) [CRC: B4A004F2] */
			else if (!strcmp(serial, "SLUS-20090")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,0027b3f4,word,3c014328",
					"patch=1,EE,0027b3f8,word,44810000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [TimeSplitters (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Tony Hawk Pro Skater 3 (NTSC-U) [CRC: EE2B2BAF] */
			else if (!strcmp(serial, "SLUS-20013")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,0017BEB8,word,3C013FF3", /* 3C013FB6 */
					"patch=1,EE,0017BEBC,word,3421CF00", /* 3421DB40 */
					"patch=1,EE,001F332C,word,3C013FF3", /* 3C013FB6 */
					"patch=1,EE,001F3330,word,3421CF00", /* 3421DB40 */
					"patch=1,EE,001F7D60,word,3C013FF3", /* 3C013FB6 */
					"patch=1,EE,001F7D64,word,3421CF00", /* 3421DB40 */
					"patch=1,EE,001F334C,word,3C013F73", /* 3C013F36 */
					"patch=1,EE,001F3350,word,3421CF00", /* 3421DB40 */
					"patch=1,EE,001F336C,word,3C014073", /* 3C014036 */
					"patch=1,EE,001F3370,word,3421CF00"  /* 3421DB40 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tony Hawk Pro Skater 3 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Trapt (NTSC-U) [CRC: DCFBB290] */
			else if (!strcmp(serial, "SLUS-21255")) /* 16:9 */
			{
				static const char *const patches[] = {
					/* 16:9 vertical fov */
					"patch=1,EE,00104954,word,3c013f40", /* 00000000 ver fov */
					"patch=1,EE,00104960,word,4481f000", /* 00000000 */
					"patch=1,EE,0010496c,word,461eb582", /* 00000000 */
					/* loading screen */
					"patch=1,EE,0029a930,word,3c024415", /* 3c0243e0 loading screen ver fov */
					/* font fix for cut-scenes */
					"patch=1,EE,001c5a4c,word,3c024190" /* 3c0241c0 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Trapt (NTSC-U)]: 16:9 (Vert-) Widescreen patch applied.\n");
			}
			/* Viewtiful Joe (NTSC-U) [CRC: 080D5356] */
			else if (!strcmp(serial, "SLUS-20951")) 
			{
				static const char *const patches[] = {
					"patch=1,EE,002b4904,word,3c01bf22",
					"patch=1,EE,002bce28,word,3c013f22"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Viewtiful Joe (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Virtua Fighter 4 (NTSC-U) [CRC: EA131B57] */
			else if (!strcmp(serial, "SLUS-20323")) /* 16:9 */
			{
				/* Patch courtesy: Elhecht */
				static const char *const patches[] = {
					/* X-Fov */
					"patch=1,EE,0026e6cc,word,3c194455", /* 3c194420 hor fov */
					"patch=1,EE,0026e6d0,word,37395555", /* 44811800 hor fov */
					"patch=1,EE,0026e6f4,word,44991800", /* 00000000 */
					/* 2044013c 00a88144 2d28a003 */
					"patch=1,EE,00249404,word,3c014456" /* 3c014420 renderfix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Virtua Fighter 4 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Virtua Fighter 4 - Evolution (NTSC-U) [CRC: C9DEF513] */
			else if (!strcmp(serial, "SLUS-20616")) 
			{
				static const char *const patches[] = {
					"patch=0,EE,003AAA74,word,3C194455", /* 3C034420 HOR FOV */
					"patch=0,EE,003AAA7C,word,37395555", /* 44830800 HOR FOV */
					"patch=0,EE,003AAA94,word,44990800", /* 00000000 */
					"patch=0,EE,00217B48,word,3C024456", /* 3C024420 RENDERFIX */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Virtua Fighter 4: Evolution (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* WipeOut Fusion (NTSC-U) [CRC: 4C2D1E6D] */
			else if (!strcmp(serial, "SLUS-20462"))  /* 16:9 */
			{
				static const char *const patches[] = {
					/* built-in ws switches */
					"patch=1,EE,2028E990,word,00000001", /* shows ws "on" under options */
					"patch=1,EE,203965CC,word,00000001", /* actual switch for internal ws mode */
					/* vert Fov */
					"patch=1,EE,2024DA74,word,3FE38E38", /* menu & singleplayer */
					"patch=1,EE,2024DAF4,word,3FE38E38", /* splitscreen left/top */
					"patch=1,EE,2024DB14,word,3FE38E38", /* splitscreen right */
					/* both FOV - ingame */
					"patch=1,EE,20282E80,word,43340000",
					/* both FOV & position - menu 3D elements
					 * ships & player heads */
					"patch=1,EE,20282A9C,word,41DBBBBC", /* ship */
					"patch=1,EE,20AAAA7C,word,43055555", /* faster driver (single player) */
					"patch=1,EE,20AAAADC,word,43055555", /* slower driver (single player) */
					"patch=1,EE,20AACF3C,word,43055555", /* faster driver (multi player) */
					"patch=1,EE,20AACF9C,word,43055555", /* slower driver (multi player) */
					/* challenge menu, page 1 (six symbols) */
					"patch=1,EE,2024705C,word,C1900000", /* y-position */
					"patch=1,EE,20247060,word,43700000", /* FOV */
					"patch=1,EE,2024706C,word,C1900000",
					"patch=1,EE,20247070,word,43700000",
					"patch=1,EE,2024707C,word,C1900000",
					"patch=1,EE,20247080,word,43700000",
					"patch=1,EE,2024708C,word,C1900000",
					"patch=1,EE,20247090,word,43700000",
					"patch=1,EE,2024709C,word,C1900000",
					"patch=1,EE,202470A0,word,43700000",
					"patch=1,EE,202470AC,word,C1900000",
					"patch=1,EE,202470B0,word,43700000",
					/* challenge menu, page 2 (symbol at the top & three medals) */
					"patch=1,EE,202470DC,word,C2700000", /* y-position */
					"patch=1,EE,202470E0,word,42F00000", /* FOV */
					"patch=1,EE,202470F4,word,42960000",
					"patch=1,EE,202470F8,word,42F00000",
					"patch=1,EE,2024710C,word,42960000",
					"patch=1,EE,20247110,word,42F00000",
					"patch=1,EE,20247124,word,42960000",
					"patch=1,EE,20247128,word,42F00000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [WipeOut Fusion (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Wreckless - The Yakuza Missions (NTSC-U) [CRC: DDE57BDF] */
			else if (!strcmp(serial, "SLUS-20431"))  /* 16:9 */
			{
				/* Patch courtesy: ElHecht */
				static const char *const patches[] = {
					"patch=1,EE,00146dc4,word,3c013f40" /* 3c013f80 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Wreckless: The Yakuza Missions (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Xenosaga Episode III: Also sprach Zarathustra (NTSC-U) [CRC: ] */
			else if (          !strcmp(serial, "SLUS-21389")
					|| !strcmp(serial, "SLUS-21417"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					/* gameplay */
					"patch=1,EE,2054FF20,extended,3fc1f080", /* original value 3f91745d */
					/* cutscenes portrait's fix */
					"patch=1,EE,00268f40,word,24020078",
					"patch=1,EE,203e4340,extended,00000174",
					"patch=1,EE,203e4360,extended,00000174"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Xenosaga Episode III: Also sprach Zarathustra (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Zathura (NTSC-U) [CRC: 844EDE02] */
			else if (!strcmp(serial, "SLUS-21336"))  /* 16:9 */
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* X-Fov
					 *4 2080246 07080046 3c0000e6 (1st) */
					"patch=1,EE,002f5684,word,0810bfa4",
					"patch=1,EE,0042fe90,word,46020842",
					"patch=1,EE,0042fe94,word,3c013f40",
					"patch=1,EE,0042fe98,word,4481f000",
					"patch=1,EE,0042fe9c,word,461e0843",
					"patch=1,EE,0042fea0,word,080bd5a2"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Zathura (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
		}
		else if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* Amplitude (NTSC-U) [CRC: 2F7B4DB8] */
			if (!strcmp(serial, "SCUS-97258"))
			{
				/* Patch courtesy: Widescreen hacks by Aced14 (MIPS code injection/FMV experiment) and 2007excalibur2007 (initial live memory discovery) */
				static const char *const patches[] = {
					"patch=1,EE,E0FF0000,extended,001001E0", /* 00000000 - Enable condition */
					"patch=1,EE,20221A88,extended,080A2564", /* E6000160 - j $00289590 - Jump to injected MIPS instructions */
					"patch=1,EE,20289590,extended,3C013F40", /* 3C050042 - lui at, $3f40 - Set $f31 register to .75 #1 */
					"patch=1,EE,20289594,extended,4481F800", /* 0200202D - mtc1 at, $f31 - Set $f31 register to .75 #2 */
					"patch=1,EE,20289598,extended,461F07C2", /* 24A5B880 - mul.s $f31, $f0, $f31 - Multiply $f0 by $f31 and store in $f31 */
					"patch=1,EE,2028959C,extended,080886A4", /* 0C0C9480 - j $00221a90 - Jump to 2 lines after the overwritten MIPS instruction */
					"patch=1,EE,202895A0,extended,E61F0160" /* 0220302D - swc1 $f31, $0160(s0) - Write $f31 into where $f0 would've been written to by the restored overwritten MIPS instruction */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Amplitude (NTSC-U)]: 16:9 Widescreen patch applied.\n");
			}
			/* Ape Escape 3 (NTSC-U) [CRC: 7571AAEE] */
			else if (!strcmp(serial, "SCUS-97501"))
			{
				static const char *const patches[] = {
					"patch=1,EE,90438e50,extended,0c10e33c",
					/* Default to builtin 16:9 widescreen mode from the start.
					 * NOTE: It won't show in the Options menu as enabled by default
					 * but it does force it regardless */
					"patch=1,EE,00649dc8,extended,00000001" /* 0 widescreen */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ape Escape 3 (NTSC-U)]: Force native widescreen mode patch applied.\n");
			}
			/* Dark Cloud (NTSC-U) [CRC: A5C05C78] */
			else if (!strcmp(serial, "SCUS-97111"))
			{
				switch (hint_widescreen)
				{
					case 4: /* 32:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,0012e228,word,3C023E90"
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Cloud (NTSC-U)]: 32:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
					case 3: /* 21:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,0012e228,word,3F023F0F"
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Cloud (NTSC-U)]: 21:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default: /* 16:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,0012e228,word,3C023F40"
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Cloud (NTSC)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Dark Cloud 2 (NTSC-U) [CRC: 1DF41F33] */
			else if (!strcmp(serial, "SCUS-97213"))
			{
				switch (hint_widescreen)
				{
					case 4: /* 32:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,00138D78,word,3F023EC0"
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Cloud 2 (NTSC)]: 32:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
					case 3: /* 21:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,00138D78,word,3F023F10"
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Cloud 2 (NTSC)]: 21:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default: /* 16:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,00138D78,word,3F023F40"
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Dark Cloud 2 (NTSC)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Downhill Domination (NTSC-U) [CRC: 5AE01D98] */
			else if (!strcmp(serial, "SCUS-97177")) /* 16:9 */
			{
				static const char *const patches[] = {
					/* default to widescreen at first run */
					"patch=1,EE,101B9EF0,extended,00004401", /* 3C01442C - Shell Menu Master X FOV */
					"patch=1,EE,101B68F4,extended,00004401", /* 3C01442C - Title Menu Master X FOV */
					"patch=1,EE,2026C5C0,extended,434FC000", /* 438A8000 - Tree Sprite Width #1 */
					"patch=1,EE,2026C700,extended,434FC000", /* 438A8000 - Tree Sprite Width #2 */
					"patch=1,EE,2026C758,extended,434FC000", /* 438A8000 - Tree Sprite Width #3 */

					"patch=1,EE,101F8510,extended,00004401", /* 3C01442C - Bike Shop Menu Goggles Default Master X FOV 
										    (added to close-up float by game engine to 
										    produce a "final" close-up X FOV) */
					"patch=1,EE,101F84F8,extended,000044D8", /* 3C014510 - Bike Shop Menu Goggles Close-up Master X FOV #1 */
					"patch=1,EE,101F84FC,extended,0000C000", /* 34218000 - Bike Shop Menu Goggles Close-up Master X FOV #2 */
					"patch=1,EE,E0030002,extended,00A095D0", /* Conditional live memory fixes for Bike Shop menu */
					"patch=1,EE,20A095D0,extended,3FAA3D71", /* 00000000 - Bike Shop Top HUD Width (Live Memory) */
					"patch=1,EE,20A09610,extended,3FAA3D71", /* 00000000 - Bike Shop Bottom HUD Width (Live Memory) */
					"patch=1,EE,20A60038,extended,C2DE0000", /* 00000000 - Bike Shop Shadow Shape/Width (Live Memory) */
					"patch=1,EE,2027F0A0,extended,3F206D3A", /* 3EF0A3D7 - Menu Master Y FOV */

					"patch=1,EE,1016BA30,extended,0000434F", /* 3C01438A - 1P P1 Master X FOV #1 */
					"patch=1,EE,1016BA34,extended,0000C000", /* 34218000 - 1P P1 Master X FOV #2 */
					"patch=1,EE,101A8D4C,extended,0000434F", /* 3C01438A - 1P P1 Replay Master X FOV #1 */
					"patch=1,EE,101A8D50,extended,0000C000", /* 34218000 - 1P P1 Replay Master X FOV #2 */
					"patch=1,EE,2027ECE0,extended,3F471C26", /* 3F15551D - 1P P1 Master Y FOV */

					"patch=1,EE,2027EF00,extended,43268000", /* 435E0000 - 2P Vertical P1 Master X FOV */
					"patch=1,EE,2027EF20,extended,3F471C26", /* 3F15551D - 2P Vertical P1 Master Y FOV */
					"patch=1,EE,2027EFC0,extended,43268000", /* 435E0000 - 2P Vertical P2 Master X FOV */
					"patch=1,EE,2027EFE0,extended,3F471C26", /* 3F15551D - 2P Vertical P2 Master Y FOV */

					"patch=1,EE,2027ED80,extended,434FC000", /* 438A8000 - 2P Horizontal P1 Master X FOV */
					"patch=1,EE,2027EDA0,extended,3F471C6A", /* 3F155550 - 2P Horizontal P1 Master Y FOV */
					"patch=1,EE,2027EE40,extended,434FC000", /* 438A8000 - 2P Horizontal P2 Master X FOV */
					"patch=1,EE,2027EE60,extended,3F471C6A", /* 3F155550 - 2P Horizontal P2 Master Y FOV */

					"patch=1,EE,2027F140,extended,431D8000", /* 43520000 - 4P P1 Master X FOV */
					"patch=1,EE,2027F160,extended,3F206D3A", /* 3EF0A3D7 - 4P P1 Master Y FOV */
					"patch=1,EE,2027F200,extended,431D8000", /* 43520000 - 4P P2 Master X FOV */
					"patch=1,EE,2027F220,extended,3F206D3A", /* 3EF0A3D7 - 4P P2 Master Y FOV */
					"patch=1,EE,2027F2C0,extended,431D8000", /* 43520000 - 4P P3 Master X FOV */
					"patch=1,EE,2027F2E0,extended,3F206D3A", /* 3EF0A3D7 - 4P P3 Master Y FOV */
					"patch=1,EE,2027F380,extended,431D8000", /* 43520000 - 4P P4 Master X FOV */
					"patch=1,EE,2027F3A0,extended,3F206D3A", /* 3EF0A3D7 - 4P P4 Master Y FOV */

					"patch=1,EE,2027ECC0,extended,434FC000" /* 438A8000 - 2-4P P1-4 Paused Master X FOV */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Downhill Domination (NTSC)]: 16:9 Widescreen patch applied.\n");
			}
			/* Extermination (NTSC-U) [CRC: 0AE679AF] */
			else if (!strcmp(serial, "SCUS-97112")) 
			{
				static const char *const patches[] = {
					"patch=1,EE,001d2978,extended,3c023f19", /* 16:9 */
					"patch=1,EE,001d297c,extended,3442999a"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Extermination (NTSC)]: 16:9 Widescreen patch applied.\n");
			}
			/* Genji - Dawn of the Samurai (NTSC-U) [CRC: D71B57F4] */
			else if (!strcmp(serial, "SCUS-97471"))
			{
				static const char *const patches[] = {
					"patch=1,EE,002e1070,word,3c014455",
					/* FMV fix by Arapapa */
					/* e043013c 00608144 00108244 */
					"patch=1,EE,002c6754,word,3c0143a8" /* 3c0143e0 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Genji: Dawn of the Samurai (NTSC)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* God of War II (NTSC-U) [CRC: 2F123FD8] */
			else if (!strcmp(serial, "SCUS-97481"))
			{
				static const char *const patches[] = {
					/* default to widescreen at first run */
					"patch=1,EE,001E45B4,word,24040001",
					"patch=1,EE,001E45B8,word,00000000",
					"patch=0,EE,0027894C,word,3c013fe3",
					"patch=0,EE,00278950,word,34218e39"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [God of War II (NTSC)]: Force native widescreen mode patch applied.\n");
			}
			/* Jak & Daxter: The Precursor Legacy (NTSC-U) [CRC: 1B3976AB] */
			else if (!strcmp(serial, "SCUS-97124"))
			{
				static const char *const patches[] = {
					"patch=1,EE,90100144,extended,0c046562",
					"patch=1,EE,202af750,extended,3f1f3b64",
					"patch=1,EE,202af6fc,extended,bf1f3b64",
					"patch=1,EE,2079f478,extended,0015120c"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Jak & Daxter: The Precursor Legacy (NTSC-U)]: Force native widescreen mode patch applied.\n");
			}
			/* Kinetica (NTSC-U) [CRC: D39C08F5] */
			else if (!strcmp(serial, "SCUS-97132")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,00172190,word,3c013f40" /* 3c013f80 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Kinetica (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* MotorStorm - Arctic Edge (U)(SCUS-97654) */
			else if (!strcmp(serial, "SCUS-97654"))
			{
				static const char *const patches[] = {
					"patch=0,EE,00295E00,word,24020002" /* 30420003 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [MotorStorm: Arctic Edge (NTSC)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
		}
		else if (!strncmp("SCES-", serial, strlen("SCES-")))
		{
			/* Amplitude (PAL) [CRC: F7DC0006] */
			if (!strcmp(serial, "SCES-51706"))
			{
				/* Patch courtesy: Widescreen hacks by Aced14 (MIPS code injection/FMV experiment) and 2007excalibur2007 (initial live memory discovery) */
				static const char *const patches[] = {
					"patch=1,EE,E0FF0000,extended,001001E0", /* 00000000 - Enable condition */
					"patch=1,EE,2022A858,extended,080A4B62", /* E6000160 - j $00292d88 - Jump to injected MIPS instructions */
					"patch=1,EE,20292D88,extended,3C013F40", /* 3C05004B - lui at, $3f40 - Set $f31 register to .75 #1 */
					"patch=1,EE,20292D8C,extended,4481F800", /* 0200202D - mtc1 at, $f31 - Set $f31 register to .75 #2 */
					"patch=1,EE,20292D90,extended,461F07C2", /* 24A53550 - mul.s $f31, $f0, $f31 - Multiply $f0 by $f31 and store in $f31 */
					"patch=1,EE,20292D94,extended,0808AA18", /* 0C0E4990 - j $0022a860 - Jump to 2 lines after the overwritten MIPS instruction */
					"patch=1,EE,20292D98,extended,E61F0160" /* 0220302D - swc1 $f31, $0160(s0) - Write $f31 into where $f0 would've been written to by the restored overwritten MIPS instruction */

				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Amplitude (PAL)]: 16:9 Widescreen patch applied.\n");
			}
			/* Dead or Alive 2 (PAL) [CRC: 7A51F86E] */
			else if (!strcmp(serial, "SCES-50003"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0021c21c,word,3c014534" /* 16:9 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dead or Alive 2 (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Dog's Life, The (PAL-M) [CRC: 531061F2] */
			else if (!strcmp(serial, "SCES-51248"))
			{
				/* Patch courtesy: PeterDelta */
				static const char *const patches[] = {
					"patch=1,EE,00AB51C0,byte,01" /* Enable native widescreen */
				};
				LOAD_PATCHES(patches);
			}
			/* Extermination (PAL-M5) [CRC: 68707E85] */
			else if (!strcmp(serial, "SCES-50240")) 
			{
				static const char *const patches[] = {
					"patch=1,EE,001D3158,word,3C023F19", /* 3C023F4C (Increases hor. axis) */
					"patch=1,EE,001D315C,word,3442AAAB" /* 3442CCCD */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Extermination (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Fantavision (PAL-M5) [CRC: ] */
			else if (!strcmp(serial, "SCES-50002")) 
			{
				/* Hor+ */
				static const char *const patches[] = {
					"patch=1,EE,901bab58,extended,0c06ea7c",
					/* X-Fov */
					"patch=1,EE,20193f50,extended,3c0143f0" /* 3c014420 */
				};
				log_cb(RETRO_LOG_INFO, "[PATCH] [Fantavision (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
				LOAD_PATCHES(patches);
			}
			/* Ridge Racer V (PAL-M5) [CRC: 5BBC2F40] */
			else if (!strcmp(serial, "SCES-50000"))
			{
				/* Patch courtesy: No.47 */
				static const char *const patches[] = {
					/* single player */
					"patch=1,EE,2033AC94,word,43C6C000", /* 44048000 - both fov, bumper camera */
					"patch=1,EE,2033ACB4,word,43C6C000", /* 44048000 - both fov, chase camera (*) */
					"patch=1,EE,2033B4C4,word,3F1D0364", /* 3EEB851F - vertical FOV */

					/* split screen */
					"patch=1,EE,2033AC90,word,43951000", /* 43C6C000 - both FOV */
					"patch=1,EE,2033B4D0,word,3F1D0364", /* 3EEB851F - vert FOV, top */
					"patch=1,EE,2033B4D4,word,3F1D0364", /* 3EEB851F - vert FOV, bottom */

					/* menu */
					"patch=1,EE,2033B580,word,3F066666" /* 3F333333 - horizontal FOV */

					/* (*) improved chase cam that shows the whole car as in other Ridge Racer games, instead of only the upper half */
					/* (*) replace 43960000 by 43C6C000 for the original chase cam */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ridge Racer V (PAL)]: 16:9 Widescreen patch applied.\n");
			}
			/* Sky Odyssey (PAL-M5) [CRC: 29B11E02] */
			else if (!strcmp(serial, "SCES-50105")) 
			{
				switch (hint_widescreen)
				{
					case 2: /* 16:10 */
						{
							static const char *const patches[] = {
								"patch=1,EE,0028ab58,word,3f555555", /* 3f800000 hor FOV */
								"patch=1,EE,00273400,word,43c00000"  /* 43a00000 increase hor FOV */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Sky Odyssey (PAL)]: 16:10 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default: /* 16:9 */
						{
							static const char *const patches[] = {
								"patch=1,EE,0028ab58,word,3f400000", /* 3f800000 hor FOV */
								"patch=1,EE,00273400,word,43d55555"  /* 43a00000 increase hor FOV */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Sky Odyssey (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Tekken Tag Tournament (PAL) [CRC: 0DD8941C] */
			else if (!strcmp(serial, "SCES-50001")) 
			{
				static const char *const patches[] = {
					"patch=1,EE,0034b014,word,3c013f40",
					"patch=1,EE,0034b018,word,44810000",
					"patch=1,EE,0034b020,word,4600c602"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken Tag Tournament (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
		}
		else if (!strncmp("SLES-", serial, strlen("SLES-")))
		{
			/* 7 Blades (PAL-M) */
			if (!strcmp(serial, "SLES-50109")) /* 16:9 */
			{
				/* Patch courtesy: Gabominated */
				static const char *const patches[] = {
					"patch=1,EE,002EF7EC,word,3C013FAB", /* 3C013F80 Y-FOV 3C013FAB */
					"patch=1,EE,002EF970,word,3C0143c0", /* 3C014400 zoom a */
					"patch=1,EE,002EF978,word,3C01433f"  /* 3C014380 zoom b */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [7 Blades (PAL)]: 16:9 (Vert-) Widescreen patch applied.\n");
			}
			/* Alias (PAL-M) [CRC: 83466553] */
			else if (!strcmp(serial, "SLES-51821"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00248138,word,3c013ec0", /* 3c013f00 hor fov */
					"patch=1,EE,001f3c70,word,3c013f40" /* 3c013f80 renderfix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Alias (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Aggressive Inline (PAL-M) [CRC: D6A0D7A5] */
			else if (!strcmp(serial, "SLES-50480"))
			{
				/* Patch courtesy: ElHecht */
				switch (hint_widescreen)
				{
					case 2: /* 16:10 */
						{
							static const char *const patches[] = {
								"patch=1,EE,0010113c,word,3c013f55", /* 00000000 hor fov */
								"patch=1,EE,0010114c,word,34215555", /* 3c013f80 hor fov */
								"patch=1,EE,00276cd4,word,3c093f55", /* 00000000 renderfix */
								"patch=1,EE,00276cd8,word,35295555" /* 00000000 renderfix */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Aggressive Inline (PAL)]: 16:10 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default:
						{
							static const char *const patches[] = {
								"patch=1,EE,0010114c,word,3c013f40", /* 3c013f80 hor fov */
								"patch=1,EE,00276cd4,word,3c093f40" /* 00000000 renderfix */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Aggressive Inline (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
				{
					static const char *const patches[] = {
						"patch=1,EE,00101794,word,0809db35", /* 46000843 */
						"patch=1,EE,00101798,word,00000000", /* e62004e8 */
						"patch=1,EE,00276cdc,word,4489f000", /* 00000000 */
						"patch=1,EE,00276ce0,word,461e0843", /* 00000000 */
						"patch=1,EE,00276ce4,word,46000843", /* 00000000 */
						"patch=1,EE,00276ce8,word,e62004e8", /* 00000000 */
						"patch=1,EE,00276cec,word,080405e6" /* 00000000 */
					};
					LOAD_PATCHES(patches);
				}
			}
			/* Armored Core 2 (PAL) [CRC: D9B48C4A] */
			else if (!strcmp(serial, "SLES-50079"))
			{
				/* Patch courtesy: Elhecht */
				switch (hint_widescreen)
				{
					case 2: /* 16:10 */
						{
							static const char *const patches[] = {
								"patch=1,EE,0028b02c,word,3c013f55", /* 00000000 hor fov gameplay */
								"patch=1,EE,0028b030,word,34215555", /* 00000000 hor fov gameplay */
								"patch=1,EE,0028b038,word,44810000", /* 00000000 */
								"patch=1,EE,0028b03c,word,4600c602", /* 00000000 */
								"patch=1,EE,001b3f2c,word,3c013f55", /* 00000000 hor fov menu */
								"patch=1,EE,001b3f30,word,34215555", /* 00000000 hor fov menu */
								"patch=1,EE,001b3f3c,word,4481f000", /* 00000000 */
								"patch=1,EE,001b3f40,word,461e6b42" /* 00000000 */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Armored Core 2 (PAL)]: 16:10 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default:
						{
							static const char *const patches[] = {
								"patch=1,EE,0028b02c,word,3c013f40", /* 00000000 hor fov gameplay */
								"patch=1,EE,0028b038,word,44810000", /* 00000000 */
								"patch=1,EE,0028b03c,word,4600c602", /* 00000000 */
								"patch=1,EE,001b3f2c,word,3c013f40", /* 00000000 hor fov menu */
								"patch=1,EE,001b3f3c,word,4481f000", /* 00000000 */
								"patch=1,EE,001b3f40,word,461e6b42" /* 00000000 */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Armored Core 2 (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Blood Omen 2: The Legacy of Kain Series (PAL) [CRC: ] */
			else if (!strcmp(serial, "SLES-50771"))
			{
				static const char *const patches[] = {
					/* gameplay (based on pavachan elf hack) */
					"patch=1,EE,00310ba0,word,3c013fe3",
					"patch=1,EE,00310ba4,word,34218e38",
					/* black border fix by nemesis2000 */
					"patch=1,EE,002d24cc,word,00000000",
					/* FMVs fix by nemesis2000 */
					"patch=1,EE,002e9380,word,240575e0",
					"patch=1,EE,002e9398,word,240a1440"
				};
				LOAD_PATCHES(patches);
			}
			/* Bloody Roar 4 (PAL) [CRC: C5DBDB45] */
			else if (!strcmp(serial, "SLES-51877")) /* 16:9 */
			{
				/* Patch courtesy: ElHecht */
				static const char *const patches[] = {
					"patch=1,EE,2060EC20,word,3FA3A283" /* 3FDA2E04 X-RES */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Bloody Roar 4 (NTSC-U)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* BMX XXX (SLES-51365) [CRC: 3A48B51C] */
			else if (!strcmp(serial, "SLES-51365"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0010107c,word,3c013f40", /* 3c013f80 hor fov */
					"patch=1,EE,00298ea4,word,3c093f40", /* 00000000 renderfix */
					"patch=1,EE,00101504,word,080a63a9", /* 46000843 */
					"patch=1,EE,00101508,word,00000000", /* e62404e8 */
					"patch=1,EE,00298eac,word,4489f000", /* 00000000 */
					"patch=1,EE,00298eb0,word,461e0843", /* 00000000 */
					"patch=1,EE,00298eb4,word,46000843", /* 00000000 */
					"patch=1,EE,00298eb8,word,e62004e8", /* 00000000 */
					"patch=1,EE,00298ebc,word,08040542" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [BMX XXX (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Breath of Fire: Dragon Quarter (PAL) [CRC: 867AB5D0] */
			else if (!strcmp(serial, "SLES-51496"))
			{
				/* Patch courtesy: nemesis2000, ElHecht */
				static const char *const patches[] = {
					"patch=1,EE,0012f71c,word,3c024306", /* 3c024333 hor val */
					"patch=1,EE,0012f720,word,3442f940", /* 3442f700 hor val */
					"patch=1,EE,0012f868,word,3c034074"  /* 3c034036 render fix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Breath of Fire: Dragon Quarter (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Britney's Dance Beat (PAL) [CRC: C0EE68EC] */
			else if (!strcmp(serial, "SLES-50947"))
			{
				static const char *const patches[] = {
					"patch=1,EE,20348134,word,3F400000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Britney's Dance Beat (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Chaos Legion (PAL) [CRC: 0ACDD053] */
			else if (!strcmp(serial, "SLES-51553")) /* 16:9 */
			{
				/* Patch courtesy: El_Patas */
				static const char *const patches[] = {
					"patch=1,EE,00243C1C,word,3C013F40", /* 46007EC6 (Increases hor. axis) */
					"patch=1,EE,00243C2C,word,4481D800", /* 00000000 */
					"patch=1,EE,00243C30,word,460FDEC2", /* 00000000 */
					/* Render fix */
					"patch=1,EE,00243D34,word,3C0243AB", /* 3C024380 */
					"patch=1,EE,00228064,word,3C023FAB" /* 3C023F80 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Chaos Legion (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Crash Bandicoot Wrath of Cortex (PAL-M) [CRC: 3A03D62F/35D70452] */
			else if (!strcmp(serial, "SLES-50386"))
			{
				if (     game_crc == 0x35D70452) /* 1.03 */
				{
					static const char *const patches[] = {
						"patch=1,EE,21D3F5A4,extended,3F100000" /* 3F400000 */
					};
					LOAD_PATCHES(patches);
				}
				else
				{
					static const char *const patches[] = {
						"patch=1,EE,21D43044,extended,3F100000" /* 3F400000 */
					};
					LOAD_PATCHES(patches);
				}
				log_cb(RETRO_LOG_INFO, "[PATCH] [Crash Bandicoot Wrath of Cortex (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Crazy Taxi (PAL-M) [CRC: C9C145BF] */
			else if (!strcmp(serial, "SLES-50215"))
			{
				/* Patch courtesy: ElHecht */
				static const char *const patches[] = {
					"patch=1,EE,0018812c,word,3c013f40", /* 3c013f80 hor fov */
					"patch=1,EE,0015e870,word,3c013f40", /* 00000000 renderfix */
					"patch=1,EE,0015e880,word,4481f000", /* 3c013f80 */
					"patch=1,EE,0015e884,word,46010d03", /* 4481a000 */
					"patch=1,EE,0015e898,word,3210ffff", /* 00000000 */
					"patch=1,EE,0015e89c,word,2610ffff", /* 3210ffff */
					"patch=1,EE,0015e8a0,word,0c066634", /* 2610ffff */
					"patch=1,EE,0015e8a4,word,0200202d", /* 0c066634 */
					"patch=1,EE,0015e8a8,word,4600a003", /* 0200202d */
					"patch=1,EE,0015e8ac,word,0200202d", /* 4600a003 */
					"patch=1,EE,0015e8b0,word,0c06660e", /* 0200202d */
					"patch=1,EE,0015e8b4,word,e7809da8", /* 0c06660e */
					"patch=1,EE,0015e8b8,word,461ea502" /* e7809da8 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Crazy Taxi (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Devil May Cry 3: Special Edition (PAL-M) [CRC: 18C9343F] */
			else if (!strcmp(serial, "SLES-54186"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					/* gameplay */
					"patch=1,EE,001ac7a4,word,3c013f40", /* hor val 1 */
					"patch=1,EE,001ac7a8,word,4481a000",
					"patch=1,EE,001ac7ac,word,3c0101d1",
					"patch=1,EE,001ac7b0,word,e43407a0",
					"patch=1,EE,001ac7b4,word,46000d03",

					"patch=1,EE,001ac814,word,3c013f40", /* hor val 2 */
					"patch=1,EE,001ac818,word,4481a000",
					"patch=1,EE,001ac81c,word,3c0101d1",
					"patch=1,EE,001ac820,word,e43407a0",
					"patch=1,EE,001ac824,word,46000d03",

					/* FMV's fix */
					"patch=1,EE,00231cb8,word,24040180", /* dolby logo */
					"patch=1,EE,00232dcc,word,240301aa", /* intro / demo */
					"patch=1,EE,002e5a8c,word,240801aa", /* cutscenes */
					"patch=1,EE,00307d4c,word,24050040", /* gallery 1 */
					"patch=1,EE,00307d54,word,24070180", /* gallery 2 */

					/* rfix 1 */
					"patch=1,EE,001ac844,word,3c0345bf", /* val 1 */
					"patch=1,EE,001ac96c,word,3c0345bf", /* val 2 */
					/* rfix 2 */
					"patch=1,EE,001acf0c,word,3c013f40", /* hor val 2 */
					"patch=1,EE,001acf24,word,44812000",
					"patch=1,EE,001acf28,word,460418c2",
					"patch=1,EE,001acf2c,word,460018c3",
					"patch=1,EE,001acf30,word,46001083",
					"patch=1,EE,001acf34,word,46000803",
					"patch=1,EE,001acf38,word,e7a30030",
					"patch=1,EE,001acf3c,word,e7a20034",

					"patch=1,EE,001acfa0,word,460418c2",
					"patch=1,EE,001acfa4,word,460018c3",
					"patch=1,EE,001acfa8,word,46001083",
					"patch=1,EE,001acfac,word,46000803",
					"patch=1,EE,001acfb0,word,e7a30030",
					"patch=1,EE,001acfb4,word,e7a20034",

					"patch=1,EE,001ad020,word,460418c2",
					"patch=1,EE,001ad024,word,460018c3",
					"patch=1,EE,001ad028,word,46001083",
					"patch=1,EE,001ad02c,word,46000803",
					"patch=1,EE,001ad030,word,e7a30030",
					"patch=1,EE,001ad034,word,e7a20034",

					"patch=1,EE,001ad0a0,word,460418c2",
					"patch=1,EE,001ad0a4,word,460018c3",
					"patch=1,EE,001ad0a8,word,46001083",
					"patch=1,EE,001ad0ac,word,46000803",
					"patch=1,EE,001ad0b0,word,e7a30030",
					"patch=1,EE,001ad0b4,word,e7a20034"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Devil May Cry 3: Special Edition (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Eternal Ring (PAL) [CRC: C5B61685] */
			else if (!strcmp(serial, "SLES-50051"))
			{
				static const char *const patches[] = {
					/* X-Fov
					 * 803f013c 00a88144 */
					"patch=1,EE,00101160,word,3c013f40" /* 3c013f80 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Eternal Ring (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Gauntlet: Seven Sorrows (PAL-M) [CRC: BBB8392E] */
			else if (!strcmp(serial, "SLES-53667"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,00446620,word,24020002" /* built in widescreen */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gauntlet: Seven Sorrows (PAL-M)]: Force native widescreen mode patch applied.\n");
			}
			/* Gradius V (PAL-M) [CRC: 0F877618] */
			else if (!strcmp(serial, "SLES-52095"))
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* X-Fov
					 * 803f033c 1855040c 06a30046 */
					"patch=1,EE,001da728,word,3c033f72", /* 3c033f80 */
					/* Render Fix
					 * 803f033c 6400a2af 0070023c */
					"patch=1,EE,002e3428,word,3c033fb0", /* 3c033f80 */
					"patch=1,EE,002e3738,word,3c033fb0", /* 3c033f80 ?? */
					"patch=1,EE,002e3e28,word,3c033fb0", /* 3c033f80 */
					"patch=1,EE,002e3f68,word,3c033fb0" /* 3c033f80 ?? */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gradius V (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* King's Field IV (PAL-M) [CRC: 401F4726 */
			else if (!strcmp(serial, "SLES-50920")) /* 16:9 */
			{
				/* Patch courtesy: pelvicthrustman */
				static const char *const patches[] = {
					"patch=1,EE,0022d3bc,word,3c013f40", /* 00000000 */
					"patch=1,EE,0022d3c0,word,44810000", /* 00000000 */
					"patch=1,EE,0022d3c8,word,4600c602" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [King's Field IV (PAL)]: 16:9 Widescreen patch applied.\n");
			}
			/* London Racer World Challenge (PAL-M) [CRC: F97680AA] */
			else if (!strcmp(serial, "SLES-51580"))
			{
				/* Patch courtesy: l-kobra */
				static const char *const patches[] = {
					"patch=1,EE,00386b70,word,3c02bf55" /* 3c02bf00 X-FOV */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [London Racer World Challenge (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Maken Shao (PAL) [CRC: 54854C71] */
			else if (!strcmp(serial, "SLES-51058"))
			{
				static const char *const patches[] = {
					"patch=1,EE,2034AC20,extended,3f533334"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Maken Shao (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Michigan: Report From Hell (PAL-M4) [CRC: DCD7104E] */
			else if (!strcmp(serial, "SLES-53073"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,e0011450,extended,0029c3b8",
					/* xxxxxxxx check 0029c3b8 matches value (0000)1450 */
					"patch=1,EE,201a70d0,extended,240400c0",
					/* 8444ca00 hor fov pal */
					"patch=1,EE,e0011446,extended,0029c3b8",
					/* xxxxxxxx check 0029c3b8 matches value (0000)1446 */
					"patch=1,EE,201a70d0,extended,240400a8",
					/* 8444ca00 hor fov ntsc */
					"patch=1,EE,00184df0,word,3c023f06" /* render fix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Michigan: Report From Hell (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Need for Speed - Hot Pursuit 2 (PAL-M6) [CRC: ] */
			else if (!strcmp(serial, "SLES-50731"))
			{
				switch (hint_widescreen)
				{
					case 3: /* 21:9 */
						{
							/* Patch courtesy: l-kobra */
							static const char *const patches[] = {
								"patch=1,EE,0032f6fc,word,3f100000", /* 3f800000 hor fov */
								"patch=1,EE,0010e994,word,46011702",
								"patch=1,EE,0032f6ec,word,3f19999a",
								"patch=1,EE,0032f850,word,3fd55555",
								"patch=1,EE,0010EDEC,short,0000",
								"patch=1,EE,0010EE0C,short,0000"
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Need for Speed: Hot Pursuit 2 (PAL)]: 21:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default:
						break;
				}
			}
			/* R-Type Final (PAL-M) [CRC: 85E994DD] */
			else if (!strcmp(serial, "SLES-51952"))
			{
				/* Patch courtesy: sergx12 */
				/* PAL conversion & black bar fix by FlatOut */
				static const char *const patches[] = {
					"patch=1,EE,0016edc0,word,3c033f40", /* menu */
					"patch=1,EE,0016fbac,word,3c043f40", /* hor */
					"patch=1,EE,0022E010,word,43d60000", /* renderfix */
					"patch=1,EE,0022E020,word,43d60000", /* renderfix */
					/* black bar fix */
					"patch=1,EE,2055EAD0,word,00000000",
					"patch=1,EE,2055EB50,word,00000000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [R-Type Final (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Rayman Revolution (PAL-M5) [CRC: 55EDA5A0] */
			else if (!strcmp(serial, "SLES-50044"))
			{
				static const char *const patches[] = {
					"patch=1,EE,0018c6a0,word,4481f000", /* 00000000 */
					"patch=1,EE,0018c6a4,word,461e0842", /* 00000000 */
					"patch=1,EE,001180ec,word,461e6303"  /* 00000000 renderfix calculation */
				};
				LOAD_PATCHES(patches);
				switch (hint_widescreen)
				{
					case 3: /* 21:9 */
						{
							/* Patch courtesy: Elhecht & ICUP321 */
							static const char *const patches[] = {
								"patch=1,EE,0018c690,word,3c013f10" /* 00000000 hor fov */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Rayman Revolution (PAL)]: 21:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
					case 2: /* 16:10 */
						{
							/* Patch courtesy: Elhecht & ICUP321 */
							static const char *const patches[] = {
								"patch=1,EE,0018c690,word,3c013f55", /* 00000000 hor fov */
								"patch=1,EE,0018c694,word,34215555" /* 00000000 hor fov */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Rayman Revolution (PAL)]: 16:10 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default: /* 16:9 */
						{
							/* Patch courtesy: Elhecht & ICUP321 */
							static const char *const patches[] = {
								"patch=1,EE,0018c690,word,3c013f40" /* 00000000 hor fov */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Rayman Revolution (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Resident Evil - Dead Aim (PAL) [CRC: F79AF536] */
			else if (!strcmp(serial, "SLES-51448")) 
			{
				/* Patch courtesy: ElHecht */
				static const char *const patches[] = {
					"patch=1,EE,00233368,word,3c1943f0", /* 3c013f80 hor fov */
					"patch=1,EE,0023336c,word,46021003", /* 44810000 */
					"patch=1,EE,0023339c,word,ac99000c" /* e482000c */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Resident Evil: Dead Aim (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Shadow of Memories (PAL) [CRC: 5F439D01] */
			else if (!strcmp(serial, "SLES-50112"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00218700,word,3c014455", /* 3c014420 */
					"patch=1,EE,00218718,word,3c013ac8"  /* 3c013b00 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Shadow of Memories (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Sitting Ducks (PAL-M5) [CRC: 6B8D216E] */
			else if (!strcmp(serial, "SLES-52116")) /* 16:9 */
			{
				/* Patch courtesy: PeterDelta */
				static const char *const patches[] = {
					"patch=1,EE,004677F0,word,3FE38E39"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sitting Ducks (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Smuggler's Run (PAL-M5) [CRC: 95416482] */
			else if (!strcmp(serial, "SLES-50061")) /* 16:9 */
			{
				/* Patch courtesy: fox140cv */
				static const char *const patches[] = {
					"patch=1,EE,0023548C,word,3C013FCC", /* 3C013F99 */
					"patch=1,EE,00235490,word,342199CD"  /* 3421999A */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Smuggler's Run (PAL)]: 16:9 Widescreen patch applied.\n");
			}
			/* Splatter Master (PAL) [CRC: 1D8EE3CF] */
			else if (!strcmp(serial, "SLES-53368")) /* 16:9 */
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* Y-Fov */
					"patch=1,EE,00138144,word,3c023f40", /* 3c023f80 */
					/* Zoom */
					"patch=1,EE,001c5fcc,word,3c024466", /* 3c024499 */
					"patch=1,EE,001c5fd4,word,34427000" /* 3442a000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Splatter Master (PAL)]: 16:9 (Vert-) Widescreen patch applied.\n");
			}
			/* Street Boyz (PAL) [CRC: BA568F6B] */
			else if (!strcmp(serial, "SLES-53407")) /* 16:9 */
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* X-Fov */
					"patch=1,EE,002284f4,word,3c013f40", /* 00000000 */
					"patch=1,EE,002284f8,word,44810000", /* 00000000 */
					"patch=1,EE,00228500,word,4600c602", /* 00000000 */
					/* Render fix #1 */
					"patch=1,EE,0015ffa0,word,3c013f2b", /* 3c013f00 Right */
					"patch=1,EE,0015ffdc,word,3c01bf2b" /* 3c01bf00 Left */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Street Boyz (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Street Fighter EX3 (PAL) [CRC: 5E13E6D6] */
			else if (!strcmp(serial, "SLES-50072")) 
			{
				/* Patch courtesy: El_Patas */
				static const char *const patches[] = {
					"patch=1,EE,002E3574,word,3C013F40", /* 00000000 (Increases hor. axis) */
					"patch=1,EE,002E3578,word,44810000", /* 00000000 */
					"patch=1,EE,002E3580,word,4600C602" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Street Fighter EX3 (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* TimeSplitters (PAL) [CRC: 288AA369] */
			else if (!strcmp(serial, "SLES-50078")) /* 16:9 */
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0027e754,word,3c014340",
					"patch=1,EE,0027e758,word,44810000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [TimeSplitters (PAL)]: 16:9 Widescreen patch applied.\n");
			}
			/* Trapt (PAL) [CRC: 2A79E058] */
			else if (!strcmp(serial, "SLES-53824")) /* 16:9 */
			{
				static const char *const patches[] = {
					/* 16:9 vertical fov */
					"patch=1,EE,00104974,word,3c013f40", /* 00000000 ver fov */
					"patch=1,EE,00104980,word,4481f000", /* 00000000 */
					"patch=1,EE,0010498c,word,461eb582", /* 00000000 */
					/* loading screen */
					"patch=1,EE,0029ce40,word,3c024415", /* 3c0243e0 loading screen ver fov */
					/* font fix for cut-scenes */
					"patch=1,EE,001c628c,word,3c024190", /* 3c0241c0 */
					/* remove black bars in cut-scenes */
					"patch=1,EE,001e4e04,word,3c020000", /* 3c024420 */
					"patch=1,EE,001e4e50,word,3c020000"  /* 3c024420 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Trapt (PAL)]: 16:9 (Vert-) Widescreen patch applied.\n");
			}
			/* Zathura (PAL-M) [CRC: B1C7FED2] */
			else if (!strcmp(serial, "SLES-53696")) /* 16:9 */
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* X-Fov */
					"patch=1,EE,002f6284,word,081037d0", /* 46020842 */
					"patch=1,EE,0040df40,word,46020842",
					"patch=1,EE,0040df44,word,3c013f40",
					"patch=1,EE,0040df48,word,4481f000",
					"patch=1,EE,0040df4c,word,461e0843",
					"patch=1,EE,0040df50,word,080bd8a2"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Zathura (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Zombie Attack (PAL) [CRC: 1CB1FCDA] */
			else if (!strcmp(serial, "SLES-53592")) /* 16:9 */
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					"patch=1,EE,0017a840,word,3c013f2a" /* 3c013f00 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Zombie Attack (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Zombie Hunters 2 (PAL-M) [CRC: 07608CA2] */
			else if (!strcmp(serial, "SLES-54569")) /* 16:9 */
			{
				/* Patch courtesy: No.47, ElHecht */
				static const char *const patches[] = {
					"patch=1,EE,00243d64,word,3c093f40", /* 00000000 hor fov */
					"patch=1,EE,00243d70,word,3c0a004a", /* 00000000 */
					"patch=1,EE,00243d74,word,ad49eb84" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Zombie Hunters 2 (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Zombie Virus (PAL) [CRC: A64DA833] */
			else if (!strcmp(serial, "SLES-54462")) /* 16:9 */
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* X-Fov */
					"patch=1,EE,00137f94,word,3c033f40", /* 3c033f80 */
					/* Render Fix */
					"patch=1,EE,00138030,word,3c023f30" /* 3c023f00 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Zombie Virus (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Zombie Zone (PAL) [CRC: A2316D03] */
			else if (!strcmp(serial, "SLES-53398")) /* 16:9 */
			{
				/* Patch courtesy: ElHecht */
				static const char *const patches[] = {
					"patch=1,EE,001d0104,word,3c013f40" /* 3c013f80 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Zombie Zone (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Zombie Zone - Other Side (PAL) [CRC: 3F73B727] */
			else if (!strcmp(serial, "SLES-54461")) /* 16:9 */
			{
				/* Patch courtesy: ElHecht */
				static const char *const patches[] = {
					"patch=1,EE,001e5a94,word,3c093f40", /* 00000000 hor fov */
					"patch=1,EE,001e5aa0,word,3c0a003a", /* 00000000 */
					"patch=1,EE,001e5aa4,word,ad49d354" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Zombie Zone - Other Side (PAL)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
		}
		else if (!strncmp("SCPS-", serial, strlen("SCPS-")))
		{
			/* Tekken 4 (NTSC-J) [CRC: 35B4028B] */
			if (!strcmp(serial, "SCPS-56006")) 
			{
				/* Patch courtesy: nemesis2000, 99skull */
				static const char *const patches[] = {
					"patch=1,EE,90280668,extended,0c0a0142",
					"patch=1,EE,20216c60,extended,3c013f40", /* gameplay */
					"patch=1,EE,201f69c0,extended,3c013f40", /* partial HUD fix */
					"patch=1,EE,2018d408,extended,3c0143d5", /* renderfix 1 */
					"patch=1,EE,20200b44,extended,3c013f40", /* renderfix 2 */
					"patch=1,EE,20200b48,extended,44810000", /* renderfix 2 */
					"patch=1,EE,20200b54,extended,46006303"  /* renderfix 2 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken 4 (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
		}
		else if (!strncmp("SLPS-", serial, strlen("SLPS-")))
		{
			/* Armored Core 2 (NTSC-J) [CRC: D00E1931] */
			if (!strcmp(serial, "SLPS-25007")) /* 16:9 */
			{
				switch (hint_widescreen)
				{
					case 2: /* 16:10 */
						{
							static const char *const patches[] = {
								"patch=1,EE,002885ec,word,3c013f55", /* 00000000 hor fov gameplay */
								"patch=1,EE,002885f0,word,34215555", /* 00000000 hor fov gameplay */
								"patch=1,EE,002885f8,word,44810000", /* 00000000 */
								"patch=1,EE,002885fc,word,4600c602", /* 00000000 */
								"patch=1,EE,001b252c,word,3c013f55", /* 00000000 hor fov menu */
								"patch=1,EE,001b2530,word,34215555", /* 00000000 hor fov menu */
								"patch=1,EE,001b253c,word,4481f000", /* 00000000 */
								"patch=1,EE,001b2540,word,461e6b42" /* 00000000 */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Armored Core 2 (NTSC-J)]: 16:10 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default:
						{
							static const char *const patches[] = {
								"patch=1,EE,002885ec,word,3c013f40", /* 00000000 hor fov gameplay */
								"patch=1,EE,002885f8,word,44810000", /* 00000000 */
								"patch=1,EE,002885fc,word,4600c602", /* 00000000 */
								"patch=1,EE,001b252c,word,3c013f40", /* 00000000 hor fov menu */
								"patch=1,EE,001b253c,word,4481f000", /* 00000000 */
								"patch=1,EE,001b2540,word,461e6b42" /* 00000000 */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Armored Core 2 (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Armored Core 3 (NTSC-J) [CRC: D00E1931] */
			else if (!strcmp(serial, "SLPS-25112")) /* 16:9 */
			{
				switch (hint_widescreen)
				{
					case 2: /* 16:10 */
						{
							static const char *const patches[] = {
								"patch=1,EE,002e6c3c,word,3c013f55", /* 00000000 hor fov */
								"patch=1,EE,002e6c40,word,34215555", /* 00000000 */
								"patch=1,EE,002e6c48,word,44810000", /* 00000000 */
								"patch=1,EE,002e6c4c,word,4600c602", /* 00000000 */
								"patch=1,EE,0026caf4,word,3c0143c1" /* 3c0143a0 renderfix */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Armored Core 3 (NTSC-J)]: 16:10 (Hor+) Widescreen patch applied.\n");
						}
						break;
					default:
						{
							static const char *const patches[] = {
								"patch=1,EE,002e6c3c,word,3c013f40", /* 00000000 hor fov */
								"patch=1,EE,002e6c48,word,44810000", /* 00000000 */
								"patch=1,EE,002e6c4c,word,4600c602", /* 00000000 */
								"patch=1,EE,0026caf4,word,3c0143d6" /* 3c0143a0 renderfix */
							};
							LOAD_PATCHES(patches);
							log_cb(RETRO_LOG_INFO, "[PATCH] [Armored Core 3 (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
						}
						break;
				}
			}
			/* Dead or Alive 2 (NTSC-J) [CRC: 7894BA09] */
			else if (!strcmp(serial, "SLPS-25002")) /* 16:9 */
			{
				/* Patch courtesy: No.47 */
				static const char *const patches[] = {
					"patch=1,EE,203B0400,word,3F400000",
					"patch=1,EE,203B0C20,word,3F400000",
					"patch=1,EE,203B1440,word,3F400000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Dead or Alive 2 (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Evergrace (NTSC-J) [CRC: 66FB2124] */
			else if (!strcmp(serial, "SLPS-25003")) /* 16:9 */
			{
				static const char *const patches[] = {
					/* 803f013c 00a88144 2e00043c */
					"patch=1,EE,002010d0,word,3c013f40", /* 3c013f80 */
					/* 803f013c 00a08144 c07b1646 */
					"patch=1,EE,00201170,word,3c013f40" /* 3c013f80 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Evergrace (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Jikuu Bouken Zentrix (NTSC-J) [CRC: F6ACFAA1] */
			else if (!strcmp(serial, "SLPS-25498")) /* 16:9 */
			{
				/* Hor+ */
				static const char *const patches[] = {
					/* X-Fov */
					"patch=1,EE,00223ad8,word,3c023f20" /* 3c023f00 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Jikuu Bouken Zentrix (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Kagero 2: Dark Illusion (NTSC-J) [CRC: 5FEE89E0) */
			else if (!strcmp(serial, "SLPS-25445")) /* 16:9 */
			{
				static const char *const patches[] = {
					/* 16:9 vertical fov */
					/* 00000000 02a80e46 83050c46 00000000 00000000 */
					"patch=1,EE,001049a4,word,3c013f40", /* 00000000 ver fov */
					"patch=1,EE,001049b0,word,4481f000", /* 00000000 */
					"patch=1,EE,001049bc,word,461eb582", /* 00000000 */
					/* loading screen
					 * e043023c 200083ac (3rd) */
					"patch=1,EE,002b6580,word,3c024415", /* 3c0243e0 loading screen ver fov */
					/* font fix for cut-scenes
					 * c041023c 00608244 (1st)  */
					"patch=1,EE,001c86f0,word,3c024190", /* 3c0241c0 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Kagero 2: Dark Illusion (NTSC-J)]: 16:9 (Vert-) Widescreen patch applied.\n");
			}
			/* Kakutou Bijin Wulong (NTSC-J) [CRC: 4A4B623A] */
			else if (!strcmp(serial, "SLPS-25657")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,203C9EB0,extended,3FE37FA9"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Kakutou Bijin Wulong (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* King's Field IV (NTSC-J) [CRC: 04C3765E] */
			else if (!strcmp(serial, "SLPS-25057")) /* 16:9 */
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					"patch=1,EE,0022c1a4,word,3c013f40", /* 00000000 */
					"patch=1,EE,0022c1a8,word,44810000", /* 00000000 */
					"patch=1,EE,0022c1b0,word,4600c602" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [King's Field IV (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Mahou no Pumpkin (NTSC-J) [CRC: 90D2D375] */
			else if (!strcmp(serial, "SLPS-20280")) /* 16:9 */
			{
				static const char *const patches[] = {
					/* X-Fov */
					/* 803f023c 1000bfff 00188244 */
					"patch=1,EE,002bf9a4,word,3c023f40" /* 3c023f80 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Mahou no Pumpkin (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* R-Type Final (NTSC-J) [CRC: D0E17D26] */
			if (!strcmp(serial, "SLPS-25247")) /* 16:9 */
			{
				static const char *const patches[] = {
					"patch=1,EE,0016f060,word,3c033f40", /* 3c033f80 */
					"patch=1,EE,0016fe4c,word,3c043f40",
					"patch=1,EE,00229f90,word,43d60000", /* renderfix  0000a043 00007043 */
					"patch=1,EE,00229fa0,word,43d60000" /* renderfix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [R-Type Final (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Ridge Racer V (NTSC-J) [CRC: 4F9C7FCF] */
			else if (!strcmp(serial, "SLPS-20001"))
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* single player 
					 * 00 80 04 44 33 33 B3 3E 33 33 B3 3E DB 0F 49 40 */
					"patch=1,EE,20330F94,word,43C6C000", /* 44048000 - both fov, bumper camera */
					/* 00 80 04 44 DB 0F 49 40 DB 0F C9 40 DB 0F 49 C0 */
					"patch=1,EE,20330FB4,word,43C6C000", /* 44048000 - both fov, chase camera (*) */
					/* 1F 85 EB 3E 7F 6A BC 3E CD CC 4C 3F 1F 85 EB 3E */
					"patch=1,EE,203317C4,word,3F1D0364", /* 3EEB851F - vertical FOV */

					/* split screen
					 * 00 C0 C6 43 00 80 04 44 33 33 B3 3E 33 33 B3 3E */
					"patch=1,EE,20330F90,word,43951000", /* 43C6C000 - both FOV */
					/* 1F 85 EB 3E 1F 85 EB 3E 67 90 0A 44 1F 85 EB 3E */
					"patch=1,EE,203317D0,word,3F1D0364", /* 3EEB851F - vert FOV, top */
					/* 1F 85 EB 3E 67 90 0A 44 1F 85 EB 3E FF FF 7F 4B */
					"patch=1,EE,203317D4,word,3F1D0364", /* 3EEB851F - vert FOV, bottom */

					/* menu */
					/* CD CC 4C 3F 7F 6A BC 3E FF FF 7F 4B CD CC CC 3D */
					"patch=1,EE,20331880,word,3F199999", /* 3F4CCCCD - horizontal FOV */
					/* (*) improved chase cam that shows the whole car as in other Ridge Racer games, instead of only the upper half */
					/* (*) replace 43960000 by 43C6C000 for the original chase cam */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ridge Racer V (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Simple 2000 Series Vol. 101 - The Oneechanpon (NTSC-J) [CRC: C5B75C7C] */
			else if (!strcmp(serial, "SLPS-20466"))
			{
				static const char *const patches[] = {
					"patch=1,EE,20495104,extended,3F400000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Simple 2000 Series Vol. 101: The Oneechanpon (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Simple 2000 Series Vol. 109 - The Taxi 2 (NTSC-J) [CRC:91A93F28] */
			else if (!strcmp(serial, "SLPS-20478"))
			{
				static const char *const patches[] = {
					/* X-Fov (Car) */
					"patch=1,EE,00155ec4,word,3c013f40", /* 00000000 */
					"patch=1,EE,00155ec8,word,44810000", /* 00000000 */
					"patch=1,EE,00155ed0,word,4600c602", /* 00000000 */
					/* X-Fov (Background) */
					"patch=1,EE,001afc74,word,3F400000" /* 3f800000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Simple 2000 Series Vol. 109 - The Taxi 2 (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Street Fighter EX3 (NTSC-J) [CRC: 63642E9F] */
			else if (!strcmp(serial, "SLPS-20003"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,002097dc,word,3c013f40", /* 00000000 */
					"patch=1,EE,002097e0,word,44810000", /* 00000000 */
					"patch=1,EE,002097e8,word,4600c602" /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Street Fighter EX3 (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Tekken Tag Tournament (NTSC-J) [CRC: 06979F19 / 40DA9BB8] */
			else if (!strcmp(serial, "SLPS-20015"))
			{
				if (     game_crc == 0x06979F19) 
				{
					static const char *const patches[] = {
						"patch=1,EE,0034b004,word,3C013F4E",
						"patch=1,EE,0034b008,word,44810000",
						"patch=1,EE,0034b010,word,4600c602"
					};
					LOAD_PATCHES(patches);
				}
				else if (game_crc == 0x40DA9BB8) 
				{
					static const char *const patches[] = {
						"patch=1,EE,0040AF4C,word,3c013f40",
						"patch=1,EE,0040AF50,word,44810000",
						"patch=1,EE,0040AF58,word,4600c602"
					};
					LOAD_PATCHES(patches);
				}
				log_cb(RETRO_LOG_INFO, "[PATCH] [Tekken Tag Tournament (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Zettai Zetsumei Toshi (NTSC-J) [CRC: 3E359E0B] */
			else if (!strcmp(serial, "SLPS-25113"))
			{
				/* Patch courtesy: LittleGiant */
				static const char *const patches[] = {
						/* 16:9 */
					"patch=1,EE,0013e3b4,word,3c023f40", /* 3c023f80 game play */
					"patch=1,EE,0013d9d4,word,3c023f40", /* 3c023f80 cutscenes fix */
					"patch=1,EE,0013e230,word,3c023f80", /* 3c023f80 cutscenes fix */
					"patch=1,EE,0025cd40,word,43E00000", /* 43a00000 (Increases hor. axis) */
					"patch=1,EE,0025cd50,word,43E00000"  /* 43a00000 render fix */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Zettai Zetsumei Toshi (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* 7 Blades (NTSC-J) [CRC: C4DD197F] */
			if (!strcmp(serial, "SLPM-65008")) /* 16:9 */
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* Zoom
					 * 0044013c 00608144 2044013c (4th) */
					"patch=1,EE,002f40c4,word,3c0143c0", /* 3c014400 */
					/* Y-Fov
					 * 3333733f 00401c46 (4th) */
					"patch=1,EE,004ef9c4,word,3fa2221d" /* 3f733333 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [7 Blades (NTSC-J)]: 16:9 (Vert-) Widescreen patch applied.\n");
			}
			/* Battle Gear 3 (NTSC-J) [CRC: AC9F1FC0] */
			else if (!strcmp(serial, "SLPM-65434")) /* 16:9 */
			{
				/* Patch courtesy: No.47 */
				static const char *const patches[] = {
					"patch=1,EE,202C9B74,word,3F400000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Battle Gear 3 (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Bloody Roar 4 (NTSC-J) [CRC: 0DA820C2] */
			else if (!strcmp(serial, "SLPM-65499")) /* 16:9 */
			{
				/* Patch courtesy: Arapapa, paul_met */
				static const char *const patches[] = {
					"patch=1,EE,00114cb8,word,3c013fe3" /* 3c013faa */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Bloody Roar 4 (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Burnout Revenge (NTSC-J) [CRC: D224D348] */
			else if (!strcmp(serial, "SLPM-66108"))
			{
				static const char *const patches[] = {
					/* Force native widescreen mode */
					"patch=0,EE,003FE6E4,extended,00000001",
					"patch=0,EE,203FE6E8,extended,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Burnout Revenge (NTSC-J)]: Force native widescreen mode patch applied.\n");
			}
			/* Chaos Legion (NTSC-J) [CRC: 5E191B9C] */
			else if (!strcmp(serial, "SLPM-65249")) /* 16:9 */
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,00242a1c,word,3c013f40",
					"patch=1,EE,00242a2c,word,4481d800",
					"patch=1,EE,00242a30,word,460fdec2",
					"patch=1,EE,00242b34,word,3c0243ab",
					"patch=1,EE,00228064,word,3c023fab",
					/* FMV's fix */
					"patch=1,EE,00328a04,word,3c026fb0",
					"patch=1,EE,00328914,word,241e77d0"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Chaos Legion (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Crash Bandicoot 4 - Sakuretsu! Majin Power NTSC-J) [CRC: F8643F9B] */
			else if (!strcmp(serial, "SLPM-62114"))
			{
				/* F8643F9B - v1.03 */
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* X-Fov
					 * 42c81446 140074e6 380060e6 */
					"patch=1,EE,001125ec,word,0805bc76",
					"patch=1,EE,0016f1d8,word,4614c042",
					"patch=1,EE,0016f1dc,word,3c013f40",
					"patch=1,EE,0016f1e0,word,4481f000",
					"patch=1,EE,0016f1e4,word,461e0842",
					"patch=1,EE,0016f1e8,word,00000000",
					"patch=1,EE,0016f1ec,word,0804497c",
					/* Render fix */
					"patch=1,EE,00114f50,word,3c013f40" /* 3c013f80 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Crash Bandicoot 4 - Sakuretsu! Majin Power (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Crazy Taxi (NTSC-J) [CRC: 8C78493E] */
			else if (!strcmp(serial, "SLPM-62102")) /* 16:9 */
			{
				/* Patch courtesy: No.47 */
				static const char *const patches[] = {
					/* Widescreen hack 16:9
					 * 713daa3f 9a99593f 0ad7233c */
					"patch=1,EE,2042AB48,word,3FE2FC93" /* 3c23d70a */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Crazy Taxi (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Devil May Cry 3: Special Edition (NTSC-J) [CRC: 79C952B0] */
			else if (!strcmp(serial, "SLPM-66160"))
			{
				/* Patch courtesy: synce */
				static const char *const patches[] = {
					"patch=1,EE,21D0DEA0,extended,3F400000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Devil May Cry 3: Special Edition (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Fate/Unlimited Codes (NTSC-J) [CRC: 3AF675BA] */
			else if (!strcmp(serial, "SLPM-55108"))
			{
				/* Patch courtesy: nemesis2000 */
				static const char *const patches[] = {
					"patch=1,EE,0019ae30,word,3c013f40",
					"patch=1,EE,0019ae34,word,44816000",
					"patch=1,EE,0019ae38,word,460c0303",
					"patch=1,EE,0019ae3c,word,0200302d",
					"patch=1,EE,0019ae40,word,0c0797bc",
					"patch=1,EE,0019ae44,word,26250340",
					"patch=1,EE,0019ae48,word,dfbf0020",
					"patch=1,EE,0019ae4c,word,7bb10010",
					"patch=1,EE,0019ae50,word,7bb00000",
					"patch=1,EE,0019ae54,word,03e00008",
					"patch=1,EE,0019ae58,word,27bd0030",

					"patch=1,EE,001e749c,word,3c023f49",
					"patch=1,EE,001e74a8,word,3443999a",

					/* FMV's fix */
					"patch=1,EE,0021dafc,word,34467100",
					"patch=1,EE,0021db54,word,34468f00"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Fate/Unlimited Codes (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Gradius V (NTSC-J) [CRC: B3F78DFA] */
			else if (!strcmp(serial, "SLPM-62462"))
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* X-Fov
					 * 803f033c 1855040c 06a30046 */
					"patch=1,EE,001d9218,word,3c033f72", /* 3c033f80 */
					/* Render Fix
					 * 803f033c 6400a2af 0070023c */
					"patch=1,EE,002e0028,word,3c033fb0", /* 3c033f80  */
					"patch=1,EE,002e0338,word,3c033fb0", /* 3c033f80 ?? */
					"patch=1,EE,002e0a28,word,3c033fb0", /* 3c033f80 */
					"patch=1,EE,002e0b68,word,3c033fb0" /* 3c033f80 ?? */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Gradius V (NTSC-J)]: 16:9 (Hor+) Widescreen patch applied.\n");
			}
			/* Ikusa Gami (NTSC-J) [CRC: 6BC8DA64] */
			else if (!strcmp(serial, "SLPM-66184"))
			{
				/* Patch courtesy: ElHecht, Little Giant */
				static const char *const patches[] = {
					"patch=1,EE,00132e6c,word,3c023f80", /* 3c023faa */
					"patch=1,EE,00132e70,word,344a0000" /* 344aaaaa */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Ikusa Gami (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Kowloon Youma Gakuenki (NTSC-J) [CRC: 57782923] */
			else if (!strcmp(serial, "SLPM-65652"))
			{
				/* Patch courtesy: Arapapa */
				static const char *const patches[] = {
					/* Game play (dungeon)
					 * FA189C3F 760BBF3F 00000000 00000000 */
					"patch=1,EE,209C1580,extended,3F6A2574", /* 3F9C18FA */
					/* Render fix
					 * 85EB513F 1F852B3F 74256A3F */
					"patch=1,EE,209C1578,extended,3f8C0000", /* 3F51EB85 */
					/* 2D Characters fix
					 * 00009B43 00000000 0000803F(*) 0000803F */
					"patch=1,EE,2077C4FC,extended,3F400000", /* 3F800000 */
					/* 0000803F */
					"patch=1,EE,2077CA5C,extended,3F400000" /* 3F800000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Kowloon Youma Gakuenki (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Sega Rally 2006 (NTSC-J) [CRC: B26172F0] */
			else if (!strcmp(serial, "SLPM-66212"))
			{
				/* Patch courtesy: VIRGIN KLM */
				static const char *const patches[] = {
					"patch=1,EE,20383AF8,word,3FC71C71",
					"patch=1,EE,20356EA0,word,3FC71C71",
					"patch=1,EE,20356EC0,word,3FC71C71",
					"patch=1,EE,2038445C,word,00000001",
					"patch=1,EE,20383C3C,word,3FC00000"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sega Rally 2006 (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Sengoku Basara (NTSC-J) [CRC: AEA1B3AD] */
			else if (!strcmp(serial, "SLPM-66058"))
			{
				/* Patch courtesy: Little Giant */
				static const char *const patches[] = {
					/* wide 16:9 */
					"patch=1,EE,0026470c,word,3c013f40", /* 00000000 */
					"patch=1,EE,00264710,word,44810000", /* 00000000 */
					"patch=1,EE,00264718,word,4600c602", /* 00000000 */
					/* render fix */
					"patch=1,EE,001afdd0,word,3c013f19", /* 3c013f4c */
					"patch=1,EE,001afdd4,word,3421999a" /* 3421cccd */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sengoku Basara (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Sengoku Basara 2 (NTSC-J) [CRC: 4961CB79] */
			else if (!strcmp(serial, "SLPM-66447"))
			{
				static const char *const patches[] = {
					"patch=1,EE,00271A2C,word,3C013F40",
					"patch=1,EE,00271A30,word,44810000",
					"patch=1,EE,00271A38,word,4600C602",
					/* render fix */
					"patch=1,EE,001F37E8,word,3C013F19",
					"patch=1,EE,001F37EC,word,3421999A"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sengoku Basara 2 (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Sengoku Basara 2 - Heroes (NTSC-J) [CRC: CA857E71] */
			else if (!strcmp(serial, "SLPM-66848"))
			{
				static const char *const patches[] = {
					/* wide 16:9 */
					"patch=1,EE,00290B0C,word,3C013F40",
					"patch=1,EE,00290B10,word,44810000",
					"patch=1,EE,00290B18,word,4600C602",
					/* render fix */
					"patch=1,EE,0010D5C4,word,3C013F19",
					"patch=1,EE,0010D5C8,word,3421999A"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Sengoku Basara 2 - Heroes (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
			/* Vampire Panic (NTSC-J) [CRC: 14DDB291 / C293DD66] */
			else if (!strcmp(serial, "SLPM-62506"))
			{
				static const char *const patches[] = {
					"patch=1,EE,001ac344,word,0809b8c0", /* e61a0264 */

					"patch=1,EE,0026e304,word,3c013f40", /* 00000000 */
					"patch=1,EE,0026e308,word,4481f000", /* 00000000 */
					"patch=1,EE,0026e30c,word,461ed682", /* 00000000 */
					"patch=1,EE,0026e310,word,e61a0264", /* 00000000 */
					"patch=1,EE,0026e314,word,0806b0d2"  /* 00000000 */
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Vampire Panic (NTSC-J)]: 16:9 Widescreen patch applied.\n");
			}
		}
	}

	if (hint_language_unlock)
	{
		if (!strncmp("SCUS-", serial, strlen("SCUS-")))
		{
			/* MotorStorm - Arctic Edge (U)(SCUS-97654) */
			if (!strcmp(serial, "SCUS-97654"))
			{
				static const char *const patches[] = {
					/* Unlock more languages */
					"patch=1,EE,0032DA04,byte,00000004"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [MotorStorm: Arctic Edge (NTSC-U)]: More languages unlocked.\n");
			}
		}
		else if (!strncmp("SLPS-", serial, strlen("SLPS-")))
		{
			/* Final Fantasy X International (NTSC-J) [CRC: 658597E2] */
			if (!strcmp(serial, "SLPS-25088"))
			{
				/* Patch courtesy: clanmash */
				static const char *const patches[] = {
					/* Forces the game to use English at all times */
					"patch=1,EE,0031ce5e,byte,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Final Fantasy X International (NTSC-J)]: Force English language applied.\n");
			}
		}
		else if (!strncmp("SLPM-", serial, strlen("SLPM-")))
		{
			/* Richard Burns Rally (NTSC-J) [CRC: 3311A6F3] */
			if (!strcmp(serial, "SLPM-66068"))
			{
				/* Patch courtesy: Nehalem */
				static const char *const patches[] = {
					/* Force the game to use English instead of Japanese */
					"patch=0,EE,20388C80,extended,474E452E",
					"patch=0,EE,203CC8D0,extended,474E4500",
					"patch=0,EE,203CCE20,extended,676E652E"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Richard Burns Rally (NTSC-J)]: Force English language applied.\n");
			}
			/* Vampire Darkstalkers Collection (NTSC-J) [CRC: BE34808C] */
			if (!strcmp(serial, "SLPM-65998"))
			{
				/* Patch courtesy: The Cutting Room Floor */
				static const char *const patches[] = {
					/* Change Main Menu Text to English */
					"patch=1,EE,01942819,extended,00000001"
				};
				LOAD_PATCHES(patches);
				log_cb(RETRO_LOG_INFO, "[PATCH] [Vampire Darkstalkers Collection (NTSC-J)]: Force English language applied.\n");
			}
		}
	}

	if (set_system_av_info)
		return 1;
	return 0;
}
