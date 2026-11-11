# Generate GameDatabaseBuiltin.c from bin/resources/GameIndex.yaml.
# CMake-script twin of tools/gen_gamedb_builtin.sh for hosts without a
# POSIX shell (MSVC runners). Output is byte-identical to the .sh script.
# Usage: cmake -DYAML=<in> -DOUT=<out.c> -P gen_gamedb_builtin.cmake
if(NOT YAML OR NOT OUT)
	message(FATAL_ERROR "gen_gamedb_builtin.cmake: -DYAML=<in> -DOUT=<out.c> required")
endif()
file(READ "${YAML}" HEX HEX)
# "0x..," per byte, 20 bytes per line (matches the awk layout).
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HEX "${HEX}")
# CMake regex has no {n} quantifier: spell out 20 repetitions.
set(_LINE "")
foreach(_i RANGE 1 20)
	string(APPEND _LINE "0x[0-9a-f][0-9a-f],")
endforeach()
string(REGEX REPLACE "(${_LINE})" "\\1\n" HEX "${HEX}")
if(NOT HEX MATCHES "\n$")
	string(APPEND HEX "\n")
endif()
file(WRITE "${OUT}.tmp"
"/* Auto-generated from bin/resources/GameIndex.yaml. Do not edit by hand.
 * Generated at build time by tools/gen_gamedb_builtin.sh.
 *
 * Embeds the PCSX2 game-compatibility database into the core so it works
 * without an external <systemdir>/resources/GameIndex.yaml. */

#include <stddef.h>

extern const unsigned char g_gameDatabaseBuiltin[];
extern const size_t g_gameDatabaseBuiltinSize;

const unsigned char g_gameDatabaseBuiltin[] = {
${HEX}};

const size_t g_gameDatabaseBuiltinSize = sizeof(g_gameDatabaseBuiltin);
")
file(RENAME "${OUT}.tmp" "${OUT}")
