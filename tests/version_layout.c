/*
 * version_layout.c
 *
 * Unit coverage for centralized version-dependent Z-machine layout rules.
 * These assertions define the project's supported-version set, deliberate V6
 * exclusion, header file-length scaling, and packed-address formulas for V3,
 * V5, V7 routine/string offsets, and V8. Keeping these rules tested here helps
 * prevent opcode or loader code from reintroducing scattered version arithmetic.
 */

#include "zmachine_version.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* Supported-version policy: V1-V5/V7/V8, with screen-oriented V6 excluded. */
    assert(zmachine_version_supported(1));
    assert(zmachine_version_supported(5));
    assert(!zmachine_version_supported(6));
    assert(zmachine_version_supported(7));
    assert(zmachine_version_supported(8));
    assert(!zmachine_version_supported(9));

    assert(strcmp(zmachine_supported_versions(), "1,2,3,4,5,7,8") == 0);

    /* Header file-length units change at V4 and again at V6/V7-era formats. */
    assert(zmachine_header_file_length(3, 100) == 200U);
    assert(zmachine_header_file_length(5, 100) == 400U);
    assert(zmachine_header_file_length(7, 100) == 800U);
    assert(zmachine_header_file_length(8, 100) == 800U);
    assert(zmachine_header_file_length(6, 100) == 0U);
    assert(zmachine_header_file_length(8, 0) == 0U);

    /* Packed routine/string addresses use the version-specific scaling rules. */
    assert(zmachine_unpack_address(3, ZM_ADDR_ROUTINE, 0x1000, 0, 0) == 0x2000U);
    assert(zmachine_unpack_address(5, ZM_ADDR_STRING, 0x1000, 0, 0) == 0x4000U);
    assert(zmachine_unpack_address(7, ZM_ADDR_ROUTINE, 0x1000, 0x20, 0x40) == 0x4100U);
    assert(zmachine_unpack_address(7, ZM_ADDR_STRING, 0x1000, 0x20, 0x40) == 0x4200U);
    assert(zmachine_unpack_address(8, ZM_ADDR_ROUTINE, 0x1000, 0, 0) == 0x8000U);

    puts("version/layout tests passed");
    return 0;
}
