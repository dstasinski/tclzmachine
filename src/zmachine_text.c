/*
 * zmachine_text.c
 *
 * Z-character, abbreviation, alphabet, ZSCII, and Unicode decoding for
 * canonical text output.
 *
 * Z-machine text is stored three 5-bit Z-characters per 16-bit word. The high
 * bit of the final word terminates a string. Depending on story version,
 * Z-characters can select alphabets, introduce abbreviations, encode line
 * breaks, or begin a 10-bit ZSCII escape. This module decodes those constructs
 * into UTF-8 for stream 1 while preserving original ZSCII bytes for memory
 * output stream 3.
 *
 * Presentation policy is intentionally not embedded here. This layer knows how
 * Z-machine character data is represented and how selected output streams must
 * receive it; optional IRC word wrapping remains above the VM at the Tcl API
 * boundary.
 */

#include "tclzmachine.h"
#include "zmachine_object.h"
#include "zmachine_text.h"

#include <stdio.h>
#include <string.h>

/* Guard malformed stories from scanning indefinitely for a terminating word. */
#define ZM_TEXT_MAX_WORDS 65536U

/* Abbreviations may recursively contain text but not further abbreviations. */
#define ZM_TEXT_MAX_RECURSION 8U

/* ZSCII's story/display-specific extra-character range. */
#define ZM_ZSCII_EXTRA_FIRST 155U
#define ZM_ZSCII_EXTRA_LAST 251U
#define ZM_ZSCII_EXTRA_COUNT 97U

/*
 * Standard default Unicode table for ZSCII 155..223. Codes 224..251 are not
 * defined by the default table. V1-V4 always use this table; V5+ use it unless
 * header-extension word 3 selects a story-provided translation table.
 */
static const uint16_t default_unicode_table[] = {
    0x00e4U, 0x00f6U, 0x00fcU, 0x00c4U, 0x00d6U, 0x00dcU,
    0x00dfU, 0x00bbU, 0x00abU, 0x00ebU, 0x00efU, 0x00ffU,
    0x00cbU, 0x00cfU, 0x00e1U, 0x00e9U, 0x00edU, 0x00f3U,
    0x00faU, 0x00fdU, 0x00c1U, 0x00c9U, 0x00cdU, 0x00d3U,
    0x00daU, 0x00ddU, 0x00e0U, 0x00e8U, 0x00ecU, 0x00f2U,
    0x00f9U, 0x00c0U, 0x00c8U, 0x00ccU, 0x00d2U, 0x00d9U,
    0x00e2U, 0x00eaU, 0x00eeU, 0x00f4U, 0x00fbU, 0x00c2U,
    0x00caU, 0x00ceU, 0x00d4U, 0x00dbU, 0x00e5U, 0x00c5U,
    0x00f8U, 0x00d8U, 0x00e3U, 0x00f1U, 0x00f5U, 0x00c3U,
    0x00d1U, 0x00d5U, 0x00e6U, 0x00c6U, 0x00e7U, 0x00c7U,
    0x00feU, 0x00f0U, 0x00deU, 0x00d0U, 0x00a3U, 0x0153U,
    0x0152U, 0x00a1U, 0x00bfU
};

/* Put the owning VM into its terminal error state with a text-layer message. */
static void text_error(ZMachine *vm, const char *message)
{
    if (!vm)
        return;

    vm->state = ZM_STATE_ERROR;
    snprintf(vm->error, sizeof(vm->error), "%s", message);
}

/* Bounds-checked byte read used for story tables referenced by text data. */
static int read_byte(const ZMachine *vm, uint32_t address, uint8_t *value)
{
    if (!vm || !vm->memory || !value ||
        (size_t)address >= vm->memory_size)
        return TCL_ERROR;

    *value = vm->memory[address];
    return TCL_OK;
}

/* Bounds-checked big-endian word read from the mutable story image. */
static int read_word(const ZMachine *vm, uint32_t address, uint16_t *value)
{
    if (!vm || !vm->memory || !value ||
        (size_t)address + 1U >= vm->memory_size)
        return TCL_ERROR;

    *value = (uint16_t)(((uint16_t)vm->memory[address] << 8) |
                       vm->memory[address + 1U]);
    return TCL_OK;
}

/*
 * Append one BMP Unicode code point as UTF-8 to the canonical output route.
 *
 * Z-machine Unicode translation-table entries are 16-bit values, so this
 * implementation only needs the one-, two-, and three-byte UTF-8 forms. Invalid
 * control/surrogate values are filtered before this helper is called.
 */
static int append_utf8(ZMachine *vm, uint16_t codepoint)
{
    char out[3];
    int len;

    if (!vm)
        return TCL_ERROR;

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
    return vm->state == ZM_STATE_ERROR ? TCL_ERROR : TCL_OK;
}

/*
 * Store one already-validated ZSCII output byte in the innermost memory stream.
 *
 * Stream 3 contains ZSCII bytes, not the UTF-8 representation used by the Tcl
 * frontend. Keeping this routing here lets an accented/custom character remain
 * e.g. byte 155 in story memory even though stream 1 renders it as Unicode.
 * The table's leading word is the current character count and is updated after
 * every successful byte write. Only dynamic memory may be modified.
 */
static int append_stream3_zscii(ZMachine *vm, uint8_t zscii)
{
    size_t table;
    size_t address;
    uint16_t count;

    if (!vm || !vm->memory || vm->stream3_depth == 0U)
        return TCL_ERROR;

    table = vm->stream3_tables[vm->stream3_depth - 1U];
    if (table + 1U >= vm->memory_size ||
        table + 1U >= (size_t)vm->static_memory_addr) {
        text_error(vm, "output stream 3 table is outside dynamic memory");
        return TCL_ERROR;
    }

    count = (uint16_t)(((uint16_t)vm->memory[table] << 8) |
                       vm->memory[table + 1U]);
    if (count == 0xffffU) {
        text_error(vm, "output stream 3 character count overflow");
        return TCL_ERROR;
    }

    address = table + 2U + (size_t)count;
    if (address >= vm->memory_size ||
        address >= (size_t)vm->static_memory_addr) {
        text_error(vm, "output stream 3 write exceeds dynamic memory");
        return TCL_ERROR;
    }

    vm->memory[address] = zscii;
    ++count;
    vm->memory[table] = (uint8_t)(count >> 8);
    vm->memory[table + 1U] = (uint8_t)count;
    return TCL_OK;
}

/* Return nonzero when a BMP value is suitable for direct UTF-8 rendering. */
static int unicode_is_printable(uint16_t codepoint)
{
    if (codepoint <= 0x001fU ||
        (codepoint >= 0x007fU && codepoint <= 0x009fU))
        return 0;

    /* UTF-16 surrogate code units are not Unicode scalar values. */
    if (codepoint >= 0xd800U && codepoint <= 0xdfffU)
        return 0;

    return 1;
}

/*
 * Resolve one extra ZSCII code through the active Unicode translation table.
 *
 * V1-V4 always use the standard default mapping. In V5+ header-extension word
 * 3 may point at a story-specific table whose first byte is its number of
 * entries, followed by 16-bit Unicode code points corresponding to ZSCII 155
 * upward. A zero pointer selects the default table.
 *
 * When *defined is false, the code lies outside the selected table. Such story
 * output is rendered safely as '?' rather than permitting undefined/control
 * data to escape to the Tcl/IRC frontend.
 */
static int extra_zscii_unicode(ZMachine *vm,
                               uint16_t zscii,
                               uint16_t *unicode,
                               int *defined)
{
    size_t index;
    uint16_t table_address = 0U;
    uint16_t extension_words = 0U;
    uint8_t count = 0U;

    if (!vm || !unicode || !defined ||
        zscii < ZM_ZSCII_EXTRA_FIRST || zscii > ZM_ZSCII_EXTRA_LAST)
        return TCL_ERROR;

    index = (size_t)(zscii - ZM_ZSCII_EXTRA_FIRST);
    *defined = 0;
    *unicode = (uint16_t)'?';

    if (vm->version >= 5U && vm->header_extension_addr != 0U) {
        if (read_word(vm, vm->header_extension_addr, &extension_words) != TCL_OK) {
            text_error(vm, "invalid Z-machine header extension table");
            return TCL_ERROR;
        }

        if (extension_words >= 3U) {
            if (read_word(vm, (uint32_t)vm->header_extension_addr + 6U,
                          &table_address) != TCL_OK) {
                text_error(vm, "truncated Unicode translation table pointer");
                return TCL_ERROR;
            }
        }
    }

    if (vm->version <= 4U || table_address == 0U) {
        if (index < sizeof(default_unicode_table) /
                        sizeof(default_unicode_table[0])) {
            *defined = 1;
            *unicode = default_unicode_table[index];
        }
        return TCL_OK;
    }

    if (read_byte(vm, table_address, &count) != TCL_OK) {
        text_error(vm, "invalid Unicode translation table address");
        return TCL_ERROR;
    }
    if (count > ZM_ZSCII_EXTRA_COUNT) {
        text_error(vm, "Unicode translation table defines too many characters");
        return TCL_ERROR;
    }
    if (index >= count)
        return TCL_OK;

    if (read_word(vm, (uint32_t)table_address + 1U + (uint32_t)(2U * index),
                  unicode) != TCL_OK) {
        text_error(vm, "truncated Unicode translation table");
        return TCL_ERROR;
    }

    *defined = 1;
    if (!unicode_is_printable(*unicode))
        *unicode = (uint16_t)'?';
    return TCL_OK;
}

/*
 * Convert one Unicode code point back to the selected ZSCII extra-character
 * table for output stream 3. Printable ASCII is handled by the caller; this
 * helper searches only ZSCII 155..251. The lookup deliberately uses the same
 * forward mapping routine as normal ZSCII output so a custom Unicode table can
 * never disagree between print_char and print_unicode.
 */
static int unicode_to_extra_zscii(ZMachine *vm,
                                  uint16_t codepoint,
                                  uint8_t *zscii,
                                  int *defined)
{
    uint16_t candidate;

    if (!vm || !zscii || !defined)
        return TCL_ERROR;

    *defined = 0;
    *zscii = (uint8_t)'?';

    for (candidate = ZM_ZSCII_EXTRA_FIRST;
         candidate <= ZM_ZSCII_EXTRA_LAST;
         ++candidate) {
        uint16_t unicode;
        int entry_defined;

        if (extra_zscii_unicode(vm, candidate,
                                &unicode, &entry_defined) != TCL_OK)
            return TCL_ERROR;
        if (entry_defined && unicode == codepoint) {
            *defined = 1;
            *zscii = (uint8_t)candidate;
            return TCL_OK;
        }
    }

    return TCL_OK;
}

/*
 * Emit one ZSCII output character through the currently selected output route.
 *
 * Normal screen/Tcl output maps ZSCII newline to '\n', passes printable ASCII,
 * and translates extra characters to UTF-8. If stream 3 is active, the Z-machine
 * requires the original single-byte ZSCII representation in story memory and
 * suppresses stream-1 rendering; therefore routing occurs before UTF-8 encoding.
 * Undefined extra-table entries become '?' consistently in either route.
 */
int zmachine_text_output_zscii(ZMachine *vm, uint16_t zscii)
{
    uint16_t unicode = 0U;
    int defined = 1;
    uint8_t stream3_byte;

    if (!vm)
        return TCL_ERROR;

    /* ZSCII null has no effect in any output stream. */
    if (zscii == 0U)
        return TCL_OK;

    if (zscii >= ZM_ZSCII_EXTRA_FIRST && zscii <= ZM_ZSCII_EXTRA_LAST) {
        if (extra_zscii_unicode(vm, zscii, &unicode, &defined) != TCL_OK)
            return TCL_ERROR;
    } else if (zscii != 13U && !(zscii >= 32U && zscii <= 126U)) {
        vm->state = ZM_STATE_ERROR;
        snprintf(vm->error, sizeof(vm->error),
                 "invalid or unsupported ZSCII output character %u (0x%02x)",
                 (unsigned)zscii, (unsigned)(zscii & 0xffU));
        return TCL_ERROR;
    }

    /* Memory stream 3 stores original ZSCII, never its UTF-8 encoding. */
    if (vm->stream3_depth > 0U) {
        stream3_byte = defined ? (uint8_t)zscii : (uint8_t)'?';
        return append_stream3_zscii(vm, stream3_byte);
    }

    if (zscii == 13U) {
        zmachine_output_append(vm, "\n", 1U);
        return vm->state == ZM_STATE_ERROR ? TCL_ERROR : TCL_OK;
    }

    if (zscii >= 32U && zscii <= 126U)
        return append_utf8(vm, zscii);

    return append_utf8(vm, defined ? unicode : (uint16_t)'?');
}

/*
 * Emit one Unicode character for EXT:11 print_unicode.
 *
 * Z-machine Unicode is limited to the BMP. Control values and surrogate code
 * units are not valid output characters. Stream 1 can carry every remaining
 * BMP scalar value because Tcl's canonical result is UTF-8. Stream 3, however,
 * is byte-oriented ZSCII: ASCII maps directly, a selected extra-character table
 * entry is used when available, and otherwise the standard requires '?'.
 */
int zmachine_text_output_unicode(ZMachine *vm, uint16_t codepoint)
{
    uint8_t zscii = (uint8_t)'?';
    int defined = 0;

    if (!vm)
        return TCL_ERROR;
    if (!unicode_is_printable(codepoint)) {
        text_error(vm, "invalid Unicode output character");
        return TCL_ERROR;
    }

    if (vm->stream3_depth > 0U) {
        if (codepoint >= 32U && codepoint <= 126U) {
            zscii = (uint8_t)codepoint;
        } else if (unicode_to_extra_zscii(vm, codepoint,
                                          &zscii, &defined) != TCL_OK) {
            return TCL_ERROR;
        } else if (!defined) {
            zscii = (uint8_t)'?';
        }
        return append_stream3_zscii(vm, zscii);
    }

    return append_utf8(vm, codepoint);
}

/*
 * Report the exact Unicode capabilities exposed by this frontend.
 *
 * UTF-8 output makes every printable BMP scalar value displayable, so bit 0 is
 * set for all non-control, non-surrogate values. The current cooperative input
 * implementation writes Tcl command bytes directly into ZSCII buffers and can
 * therefore promise Unicode keyboard input only for the shared printable ASCII
 * subset; bit 1 is deliberately conservative until UTF-8 input decoding exists.
 */
uint16_t zmachine_text_unicode_capabilities(uint16_t codepoint)
{
    uint16_t capabilities = 0U;

    if (!unicode_is_printable(codepoint))
        return 0U;

    capabilities |= 0x0001U;
    if (codepoint >= 32U && codepoint <= 126U)
        capabilities |= 0x0002U;
    return capabilities;
}

/*
 * Translate an ordinary alphabet Z-character (6..31) to ZSCII.
 *
 * A0 and A1 are the lowercase/uppercase Latin alphabets. A2 differs slightly
 * between V1 and later versions. V5+ may replace all three 26-character
 * alphabets with the 78-byte table whose address is stored in header bytes
 * 0x34..0x35. Shift/control Z-characters are handled by decode_internal() and
 * must never reach this helper.
 */
static int alphabet_zscii(const ZMachine *vm,
                          uint8_t alphabet,
                          uint8_t zchar,
                          uint16_t *zscii)
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

    *zscii =
        (uint8_t)((vm->version == 1U ? a2_v1 : a2_v2)[zchar - 6U]);
    return TCL_OK;
}

/*
 * Compute the V1/V2 relative alphabet selected by a shift control.
 * zchar 2 advances one alphabet; zchar 3 advances two, all modulo A0/A1/A2.
 * V3+ uses fixed A1/A2 temporary shifts and does not call this helper.
 */
static uint8_t shift_alphabet(uint8_t current, uint8_t zchar)
{
    if (zchar == 2U)
        return (uint8_t)((current + 1U) % 3U);

    return (uint8_t)((current + 2U) % 3U);
}

/*
 * Decode one packed Z-text string beginning at address.
 *
 * Each loop iteration reads a 16-bit packed word and expands its three 5-bit
 * Z-characters. The small state machine tracks four independent constructs:
 *
 * - current_alphabet is the persistent alphabet used only by V1/V2 shift-lock;
 * - temporary_alphabet is a one-character shift (V1/V2 relative, V3+ A1/A2);
 * - abbreviation_pending remembers that the next Z-character selects one of
 *   32 entries in an abbreviation set;
 * - zscii_pending collects the two five-bit pieces of a 10-bit ZSCII escape.
 *
 * Abbreviation strings are decoded recursively with allow_abbreviations=0,
 * enforcing the specification rule that abbreviation text cannot itself invoke
 * another abbreviation. depth and the word limit also protect malformed story
 * data from recursive or unterminated decoding.
 *
 * If next_address is non-NULL it receives the byte immediately after the final
 * packed word. Inline `print`/`print_ret` opcodes use that continuation as the
 * next instruction address; address/packed/object-name printing does not need it.
 */
static int decode_internal(ZMachine *vm,
                           uint32_t address,
                           int allow_abbreviations,
                           unsigned depth,
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

    if (!vm || !vm->memory)
        return TCL_ERROR;

    if (depth > ZM_TEXT_MAX_RECURSION) {
        text_error(vm, "Z-text abbreviation recursion limit exceeded");
        return TCL_ERROR;
    }

    for (;;) {
        uint16_t word;
        uint8_t zchars[3];
        unsigned i;
        int final_word;

        if (++words > ZM_TEXT_MAX_WORDS ||
            read_word(vm, cursor, &word) != TCL_OK) {
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

            /* The selector after an abbreviation marker chooses its entry. */
            if (abbreviation_pending) {
                uint16_t packed;
                uint32_t entry =
                    (uint32_t)vm->abbreviations_addr +
                    2U * (uint32_t)(32U * (abbreviation_set - 1U) + z);

                if (!allow_abbreviations) {
                    text_error(vm,
                               "abbreviation used inside abbreviation text");
                    return TCL_ERROR;
                }

                if (read_word(vm, entry, &packed) != TCL_OK) {
                    text_error(vm,
                               "invalid Z-text abbreviation table entry");
                    return TCL_ERROR;
                }

                /* Abbreviation addresses are always packed in units of 2. */
                if (decode_internal(vm,
                                    (uint32_t)packed * 2U,
                                    0,
                                    depth + 1U,
                                    NULL) != TCL_OK)
                    return TCL_ERROR;

                abbreviation_pending = 0;
                continue;
            }

            /* A2/6 in V2+ consumes the next two Z-characters as 10-bit ZSCII. */
            if (zscii_pending != 0) {
                if (zscii_pending == 2) {
                    zscii_value = (uint16_t)z << 5;
                    zscii_pending = 1;
                } else {
                    zscii_value = (uint16_t)(zscii_value | z);
                    if (zmachine_text_output_zscii(vm,
                                                   zscii_value) != TCL_OK)
                        return TCL_ERROR;
                    zscii_pending = 0;
                }
                continue;
            }

            /* Z-character 0 is a space in every supported version. */
            if (z == 0U) {
                if (zmachine_text_output_zscii(vm, 32U) != TCL_OK)
                    return TCL_ERROR;
                if (vm->version >= 3U)
                    has_temporary_alphabet = 0;
                continue;
            }

            /* V1 alone assigns Z-character 1 directly to newline. */
            if (vm->version == 1U && z == 1U) {
                if (zmachine_text_output_zscii(vm, 13U) != TCL_OK)
                    return TCL_ERROR;
                continue;
            }

            /* V2 has one abbreviation set; V3+ has three sets (zchars 1..3). */
            if ((vm->version >= 3U && z >= 1U && z <= 3U) ||
                (vm->version == 2U && z == 1U)) {
                abbreviation_pending = 1;
                abbreviation_set = z;
                continue;
            }

            /* V1/V2 zchars 2/3 temporarily shift relative to current alphabet. */
            if (vm->version <= 2U && (z == 2U || z == 3U)) {
                temporary_alphabet = shift_alphabet(current_alphabet, z);
                has_temporary_alphabet = 1;
                continue;
            }

            /* V1/V2 zchars 4/5 shift-lock the current alphabet persistently. */
            if (vm->version <= 2U && (z == 4U || z == 5U)) {
                current_alphabet =
                    shift_alphabet(current_alphabet, (uint8_t)(z - 2U));
                has_temporary_alphabet = 0;
                continue;
            }

            /* V3+ zchars 4 and 5 temporarily select A1 and A2 respectively. */
            if (vm->version >= 3U && (z == 4U || z == 5U)) {
                temporary_alphabet = (uint8_t)(z - 3U);
                has_temporary_alphabet = 1;
                continue;
            }

            alphabet = has_temporary_alphabet
                           ? temporary_alphabet
                           : current_alphabet;

            /* A2/6 begins a two-Z-character 10-bit ZSCII escape in V2+. */
            if (alphabet == 2U && z == 6U && vm->version >= 2U) {
                zscii_pending = 2;
                if (vm->version >= 3U)
                    has_temporary_alphabet = 0;
                continue;
            }

            /* A2/7 is the newline entry in the standard V2+ alphabet. */
            if (alphabet == 2U && z == 7U && vm->version >= 2U) {
                if (zmachine_text_output_zscii(vm, 13U) != TCL_OK)
                    return TCL_ERROR;
                if (vm->version >= 3U)
                    has_temporary_alphabet = 0;
                continue;
            }

            if (alphabet_zscii(vm, alphabet, z, &out) != TCL_OK) {
                text_error(vm, "invalid Z-text alphabet table reference");
                return TCL_ERROR;
            }

            if (zmachine_text_output_zscii(vm, out) != TCL_OK)
                return TCL_ERROR;

            /* Every temporary shift expires after the character it affects. */
            if (has_temporary_alphabet)
                has_temporary_alphabet = 0;
        }

        if (final_word)
            break;
    }

    if (next_address)
        *next_address = cursor;

    return TCL_OK;
}

/*
 * Decode a Z-text string at an unpacked byte address.
 * next_address may be NULL; otherwise it receives the byte following the final
 * encoded word, which is needed by inline print instructions.
 */
int zmachine_text_print(ZMachine *vm,
                        uint32_t address,
                        uint32_t *next_address)
{
    return decode_internal(vm, address, 1, 0U, next_address);
}

/* Decode a string whose address uses the current version's packed-string rule. */
int zmachine_text_print_packed(ZMachine *vm, uint16_t packed_address)
{
    uint32_t address;

    if (!vm)
        return TCL_ERROR;

    address = zmachine_unpack_string_address(vm, packed_address);
    return zmachine_text_print(vm, address, NULL);
}

/*
 * Print an object's short name from the beginning of its property table.
 *
 * The first property-table byte is the short-name length in packed words. A
 * zero length therefore produces no text; otherwise decoding begins immediately
 * after that byte. The property subsystem owns version-specific object layout,
 * so this text routine never needs to know where the property pointer lives.
 */
int zmachine_text_print_object_name(ZMachine *vm, uint16_t object)
{
    uint32_t table;
    uint8_t words;

    if (!vm)
        return TCL_ERROR;

    if (zmachine_object_property_table(vm, object, &table) != TCL_OK)
        return TCL_ERROR;

    if (read_byte(vm, table, &words) != TCL_OK) {
        text_error(vm, "invalid object short-name table");
        return TCL_ERROR;
    }

    if (words == 0U)
        return TCL_OK;

    return zmachine_text_print(vm, table + 1U, NULL);
}
