#include "tclzmachine.h"
#include "zmachine_object.h"
#include "zmachine_text.h"

#include <stdio.h>
#include <string.h>

#define ZM_TEXT_MAX_WORDS 65536U
#define ZM_TEXT_MAX_RECURSION 8U

static void text_error(ZMachine *vm, const char *message)
{
    if (!vm) return;
    vm->state = ZM_STATE_ERROR;
    snprintf(vm->error, sizeof(vm->error), "%s", message);
}

static int read_byte(const ZMachine *vm, uint32_t address, uint8_t *value)
{
    if (!vm || !vm->memory || !value || (size_t)address >= vm->memory_size)
        return TCL_ERROR;
    *value = vm->memory[address];
    return TCL_OK;
}

static int read_word(const ZMachine *vm, uint32_t address, uint16_t *value)
{
    if (!vm || !vm->memory || !value || (size_t)address + 1U >= vm->memory_size)
        return TCL_ERROR;
    *value = (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                       vm->memory[address + 1U]);
    return TCL_OK;
}

static int append_utf8(ZMachine *vm, uint16_t codepoint)
{
    char out[3];
    int len;

    if (!vm) return TCL_ERROR;

    if (codepoint <= 0x7fU) {
        out[0] = (char)codepoint;
        len = 1;
    } else if (codepoint <= 0x7ffU) {
        out[0] = (char)(0xc0U | (codepoint >> 6));
        out[1] = (char)(0x80U | (codepoint & 0x3fU));
        len = 2;
    } else {
        out[0] = (char)(0xe0U | (codepoint >> 12));
        out[1] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        out[2] = (char)(0x80U | (codepoint & 0x3fU));
        len = 3;
    }

    zmachine_output_append(vm, out, (size_t)len);
    return TCL_OK;
}

int zmachine_text_output_zscii(ZMachine *vm, uint16_t zscii)
{
    if (!vm) return TCL_ERROR;

    if (zscii == 0U) return TCL_OK;
    if (zscii == 13U) {
        zmachine_output_append(vm, "\n", 1U);
        return TCL_OK;
    }
    if (zscii >= 32U && zscii <= 126U)
        return append_utf8(vm, zscii);

    /*
     * Extra-character Unicode translation is intentionally conservative in
     * this milestone. Classic Infocom text is overwhelmingly ASCII; later
     * Unicode translation-table support can map ZSCII 155..251 precisely.
     */
    if (zscii >= 155U && zscii <= 251U) {
        zmachine_output_append(vm, "?", 1U);
        return TCL_OK;
    }

    text_error(vm, "invalid or unsupported ZSCII output character");
    return TCL_ERROR;
}

static int alphabet_zscii(const ZMachine *vm, uint8_t alphabet,
                          uint8_t zchar, uint16_t *zscii)
{
    static const char a0[] = "abcdefghijklmnopqrstuvwxyz";
    static const char a1[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char a2_v1[] = " 0123456789.,!?_#'\"/\\<-:()";
    static const char a2_v2[] = " \n0123456789.,!?_#'\"/\\-:()";
    uint16_t table_addr = 0U;
    uint32_t index;
    uint8_t value;

    if (!vm || !zscii || alphabet > 2U || zchar < 6U || zchar > 31U)
        return TCL_ERROR;

    if (vm->version >= 5U && vm->memory_size > 0x35U) {
        table_addr = (uint16_t)(((uint16_t)vm->memory[0x34] << 8) |
                               vm->memory[0x35]);
    }

    if (table_addr != 0U) {
        index = (uint32_t)table_addr + (uint32_t)alphabet * 26U +
                (uint32_t)(zchar - 6U);
        if (read_byte(vm, index, &value) != TCL_OK)
            return TCL_ERROR;
        *zscii = value;
        return TCL_OK;
    }

    if (alphabet == 0U) {
        *zscii = (uint8_t)a0[zchar - 6U];
        return TCL_OK;
    }
    if (alphabet == 1U) {
        *zscii = (uint8_t)a1[zchar - 6U];
        return TCL_OK;
    }

    *zscii = (uint8_t)((vm->version == 1U ? a2_v1 : a2_v2)[zchar - 6U]);
    return TCL_OK;
}

static uint8_t shift_alphabet(uint8_t current, uint8_t zchar)
{
    if (zchar == 2U)
        return (uint8_t)((current + 1U) % 3U);
    return (uint8_t)((current + 2U) % 3U);
}

static int decode_internal(ZMachine *vm, uint32_t address,
                           int allow_abbreviations, unsigned depth,
                           uint32_t *next_address)
{
    uint32_t cursor = address;
    uint32_t words = 0U;
    uint8_t current_alphabet = 0U;
    uint8_t temporary_alphabet = 0U;
    int has_temporary_alphabet = 0;
    int abbreviation_pending = 0;
    uint8_t abbreviation_set = 0U;
    int zscii_pending = 0;
    uint16_t zscii_value = 0U;

    if (!vm || !vm->memory) return TCL_ERROR;
    if (depth > ZM_TEXT_MAX_RECURSION) {
        text_error(vm, "Z-text abbreviation recursion limit exceeded");
        return TCL_ERROR;
    }

    for (;;) {
        uint16_t word;
        uint8_t zchars[3];
        unsigned i;
        int final_word;

        if (++words > ZM_TEXT_MAX_WORDS || read_word(vm, cursor, &word) != TCL_OK) {
            text_error(vm, "unterminated or truncated Z-text string");
            return TCL_ERROR;
        }
        cursor += 2U;
        final_word = (word & 0x8000U) != 0U;
        zchars[0] = (uint8_t)((word >> 10) & 0x1fU);
        zchars[1] = (uint8_t)((word >> 5) & 0x1fU);
        zchars[2] = (uint8_t)(word & 0x1fU);

        for (i = 0U; i < 3U; ++i) {
            uint8_t z = zchars[i];
            uint8_t alphabet;
            uint16_t out;

            if (abbreviation_pending) {
                uint16_t packed;
                uint32_t entry = (uint32_t)vm->abbreviations_addr +
                                 2U * (uint32_t)(32U * (abbreviation_set - 1U) + z);
                if (!allow_abbreviations) {
                    text_error(vm, "abbreviation used inside abbreviation text");
                    return TCL_ERROR;
                }
                if (read_word(vm, entry, &packed) != TCL_OK) {
                    text_error(vm, "invalid Z-text abbreviation table entry");
                    return TCL_ERROR;
                }
                if (decode_internal(vm, (uint32_t)packed * 2U, 0, depth + 1U, NULL) != TCL_OK)
                    return TCL_ERROR;
                abbreviation_pending = 0;
                continue;
            }

            if (zscii_pending != 0) {
                if (zscii_pending == 2) {
                    zscii_value = (uint16_t)z << 5;
                    zscii_pending = 1;
                } else {
                    zscii_value = (uint16_t)(zscii_value | z);
                    if (zmachine_text_output_zscii(vm, zscii_value) != TCL_OK)
                        return TCL_ERROR;
                    zscii_pending = 0;
                }
                continue;
            }

            if (z == 0U) {
                if (zmachine_text_output_zscii(vm, 32U) != TCL_OK) return TCL_ERROR;
                if (vm->version >= 3U) has_temporary_alphabet = 0;
                continue;
            }

            if (vm->version == 1U && z == 1U) {
                if (zmachine_text_output_zscii(vm, 13U) != TCL_OK) return TCL_ERROR;
                continue;
            }

            if ((vm->version >= 3U && z >= 1U && z <= 3U) ||
                (vm->version == 2U && z == 1U)) {
                abbreviation_pending = 1;
                abbreviation_set = z;
                continue;
            }

            if (vm->version <= 2U && (z == 2U || z == 3U)) {
                temporary_alphabet = shift_alphabet(current_alphabet, z);
                has_temporary_alphabet = 1;
                continue;
            }
            if (vm->version <= 2U && (z == 4U || z == 5U)) {
                current_alphabet = shift_alphabet(current_alphabet,
                                                  (uint8_t)(z - 2U));
                has_temporary_alphabet = 0;
                continue;
            }
            if (vm->version >= 3U && (z == 4U || z == 5U)) {
                temporary_alphabet = (uint8_t)(z - 3U);
                has_temporary_alphabet = 1;
                continue;
            }

            alphabet = has_temporary_alphabet ? temporary_alphabet : current_alphabet;

            if (alphabet == 2U && z == 6U && vm->version >= 2U) {
                zscii_pending = 2;
                if (vm->version >= 3U) has_temporary_alphabet = 0;
                continue;
            }
            if (alphabet == 2U && z == 7U && vm->version >= 2U) {
                if (zmachine_text_output_zscii(vm, 13U) != TCL_OK) return TCL_ERROR;
                if (vm->version >= 3U) has_temporary_alphabet = 0;
                continue;
            }

            if (alphabet_zscii(vm, alphabet, z, &out) != TCL_OK) {
                text_error(vm, "invalid Z-text alphabet table reference");
                return TCL_ERROR;
            }
            if (zmachine_text_output_zscii(vm, out) != TCL_OK) return TCL_ERROR;

            if (has_temporary_alphabet)
                has_temporary_alphabet = 0;
        }

        if (final_word) break;
    }

    if (next_address) *next_address = cursor;
    return TCL_OK;
}

int zmachine_text_print(ZMachine *vm, uint32_t address, uint32_t *next_address)
{
    return decode_internal(vm, address, 1, 0U, next_address);
}

int zmachine_text_print_packed(ZMachine *vm, uint16_t packed_address)
{
    uint32_t address;
    if (!vm) return TCL_ERROR;
    address = zmachine_unpack_string_address(vm, packed_address);
    return zmachine_text_print(vm, address, NULL);
}

int zmachine_text_print_object_name(ZMachine *vm, uint16_t object)
{
    uint32_t table;
    uint8_t words;

    if (!vm) return TCL_ERROR;
    if (zmachine_object_property_table(vm, object, &table) != TCL_OK)
        return TCL_ERROR;
    if (read_byte(vm, table, &words) != TCL_OK) {
        text_error(vm, "invalid object short-name table");
        return TCL_ERROR;
    }
    if (words == 0U) return TCL_OK;
    return zmachine_text_print(vm, table + 1U, NULL);
}
