/* (features_cpu.c's one call out, which xxh3's dispatch never makes) */
void retro_sleep_us(unsigned us);
void retro_sleep_us(unsigned us) { (void)us; }
