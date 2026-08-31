/*
 * zmachine_input.c
 *
 * Line-input storage and dictionary tokenization for the cooperative,
 * text-only Z-machine runtime.
 *
 * Tcl supplies an entire command line at once. This module converts that line
 * into the exact text-buffer representation expected by the active Z-machine
 * version, lowercases it for dictionary matching, and optionally fills the
 * story's parse buffer. The same lexical engine is also exposed to the V5+
 * `tokenise` opcode so read-time parsing and explicit parsing cannot drift.
 * It intentionally contains no terminal, filesystem, or IRC behavior.
 */

#include "tclzmachine.h"
#include "zmachine_input.h"

#include <ctype.h>
#include <string.h>

/* Read one big-endian 16-bit value from a byte array. */
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/*
 * Write one byte to story dynamic memory.
 *
 * Z-machine input buffers and parse buffers are writable story structures, so
 * writes at or beyond the static-memory boundary must be rejected.
 */
static int write_byte(ZMachine *vm, uint32_t addr, uint8_t value)
{
    if (!vm || !vm->memory || addr >= vm->memory_size ||
        addr >= vm->static_memory_addr)
        return TCL_ERROR;

    vm->memory[addr] = value;
    return TCL_OK;
}

/* Write one big-endian 16-bit word to story dynamic memory. */
static int write_word(ZMachine *vm, uint32_t addr, uint16_t value)
{
    if (write_byte(vm, addr, (uint8_t)(value >> 8)) != TCL_OK)
        return TCL_ERROR;
    return write_byte(vm, addr + 1U, (uint8_t)value);
}

/*
 * Map one lower-case input character to the default Z-machine alphabets.
 *
 * Return 0 for alphabet A0, 2 for A2, or -1 when the character must be encoded
 * as a 10-bit ZSCII escape. Input is lowercased before this helper is called,
 * so A1 is not needed for ordinary dictionary matching.
 */
static int alphabet_index(unsigned char c, uint8_t *zchar)
{
    static const char a2[] = " ^0123456789.,!?_#'\"/\\-:()";
    const char *p;

    if (c >= 'a' && c <= 'z') {
        *zchar = (uint8_t)(6U + (c - 'a'));
        return 0;
    }

    p = strchr(a2, (int)c);
    if (p && p >= a2 + 2) {
        *zchar = (uint8_t)(6U + (p - (a2 + 2)));
        return 2;
    }

    return -1;
}

/*
 * Encode a token into the fixed-width dictionary key format.
 *
 * V1-V3 dictionaries compare the first 6 Z-characters, stored in two words
 * (4 bytes). V4+ compare 9 Z-characters in three words (6 bytes). Unused
 * positions are padded with Z-character 5, and the high bit of the final word
 * marks the end of the encoded string.
 */
static void encode_dictionary_word(uint8_t version, const char *word,
                                   uint8_t *out, size_t out_len)
{
    uint8_t zchars[9];
    size_t zmax = version <= 3 ? 6U : 9U;
    size_t zi = 0U, i;
    uint16_t words[3] = {0, 0, 0};

    memset(zchars, 5, sizeof(zchars));

    for (i = 0U; word[i] && zi < zmax; ++i) {
        unsigned char c = (unsigned char)tolower((unsigned char)word[i]);
        uint8_t z;
        int alphabet = alphabet_index(c, &z);

        if (alphabet == 0) {
            zchars[zi++] = z;
        } else if (alphabet == 2 && zi + 1U < zmax) {
            /* Shift into A2 for one character. */
            zchars[zi++] = (version <= 2) ? 3U : 5U;
            zchars[zi++] = z;
        } else if (zi + 3U < zmax) {
            /* A2 Z-character 6 introduces a 10-bit ZSCII literal. */
            zchars[zi++] = (version <= 2) ? 3U : 5U;
            zchars[zi++] = 6U;
            zchars[zi++] = (uint8_t)((c >> 5) & 0x1fU);
            zchars[zi++] = (uint8_t)(c & 0x1fU);
        }
    }

    for (i = 0U; i < zmax / 3U; ++i) {
        words[i] = (uint16_t)(((uint16_t)zchars[i * 3U] << 10) |
                              ((uint16_t)zchars[i * 3U + 1U] << 5) |
                              zchars[i * 3U + 2U]);
        if (i == zmax / 3U - 1U)
            words[i] |= 0x8000U;

        out[i * 2U] = (uint8_t)(words[i] >> 8);
        out[i * 2U + 1U] = (uint8_t)words[i];
    }

    /* out_len documents the caller's buffer contract for future expansion. */
    (void)out_len;
}

/*
 * Look up one already-tokenized word in a selected story dictionary.
 *
 * The dictionary header supplies a separator list, entry width, and signed
 * entry count. A negative count indicates an unsorted table. Linear search is
 * valid for both sorted and unsorted dictionaries and is especially useful for
 * `tokenise`, whose optional user dictionary may be modified during play.
 *
 * Return the dictionary entry address on success or zero when the token is not
 * present. A zero address is the value required in the parse buffer for an
 * unrecognized word.
 */
static uint16_t dictionary_lookup(const ZMachine *vm,
                                  uint16_t dictionary_addr,
                                  const char *word)
{
    uint32_t d = dictionary_addr;
    uint8_t separators, entry_len;
    int entry_count;
    uint32_t entries;
    uint8_t encoded[6];
    size_t key_len = vm->version <= 3 ? 4U : 6U;
    int i;

    if (!vm->memory || d == 0U || d >= vm->memory_size)
        return 0U;

    separators = vm->memory[d++];
    if ((size_t)d + separators > vm->memory_size)
        return 0U;
    d += separators;
    if ((size_t)d + 3U > vm->memory_size)
        return 0U;

    entry_len = vm->memory[d++];
    entry_count = (int16_t)be16(vm->memory + d);
    d += 2U;
    entries = d;

    if (entry_count < 0)
        entry_count = -entry_count;
    if ((size_t)entry_len < key_len)
        return 0U;

    memset(encoded, 0, sizeof(encoded));
    encode_dictionary_word(vm->version, word, encoded, key_len);

    for (i = 0; i < entry_count; ++i) {
        uint32_t addr = entries + (uint32_t)i * entry_len;

        if ((size_t)addr + key_len > vm->memory_size)
            break;
        if (memcmp(vm->memory + addr, encoded, key_len) == 0) {
            /* Parse-buffer dictionary addresses are 16-bit by definition. */
            if (addr > 0xffffU)
                return 0U;
            return (uint16_t)addr;
        }
    }

    return 0U;
}

/* Return nonzero when c is one of the selected dictionary's separators. */
static int is_separator(const ZMachine *vm,
                        uint16_t dictionary_addr,
                        unsigned char c)
{
    uint32_t d = dictionary_addr;
    uint8_t count, i;

    if (!vm->memory || d == 0U || d >= vm->memory_size)
        return 0;

    count = vm->memory[d++];
    for (i = 0U; i < count && (size_t)d + i < vm->memory_size; ++i) {
        if (vm->memory[d + i] == c)
            return 1;
    }
    return 0;
}

/*
 * Split one input line into dictionary tokens and fill the parse buffer.
 *
 * Spaces delimit ordinary words but are not themselves tokens. A dictionary
 * separator is emitted as a one-character token. Each standard parse entry is
 * four bytes: dictionary address, token length, and byte offset into the story
 * text buffer. text_offset is 1 in V1-V4 and 2 in V5+ because the physical
 * text-buffer layouts differ between those version families.
 *
 * When preserve_unrecognized is true, an unknown word leaves all four bytes of
 * its existing parse slot untouched while still incrementing the token count.
 * This is the special flag behavior of the V5+ `tokenise` opcode.
 */
static int tokenize(ZMachine *vm,
                    uint16_t parse_buffer,
                    const char *line,
                    size_t len,
                    uint8_t text_offset,
                    uint16_t dictionary_addr,
                    int preserve_unrecognized)
{
    uint8_t max_words, count = 0U;
    size_t i = 0U;

    /* A zero parse-buffer address is harmless for read's optional parse arg. */
    if (parse_buffer == 0U)
        return TCL_OK;
    if ((size_t)parse_buffer + 1U >= vm->memory_size)
        return TCL_ERROR;

    max_words = vm->memory[parse_buffer];
    if (write_byte(vm, (uint32_t)parse_buffer + 1U, 0U) != TCL_OK)
        return TCL_ERROR;

    while (i < len && count < max_words) {
        char token[128];
        size_t start, key_len = 0U, token_len;
        uint16_t dict_addr;
        uint32_t entry_addr;

        while (i < len && line[i] == ' ')
            ++i;
        if (i >= len)
            break;

        start = i;
        if (is_separator(vm, dictionary_addr, (unsigned char)line[i])) {
            token[key_len++] = line[i++];
        } else {
            while (i < len && line[i] != ' ' &&
                   !is_separator(vm, dictionary_addr,
                                 (unsigned char)line[i])) {
                if (key_len + 1U < sizeof(token))
                    token[key_len++] =
                        (char)tolower((unsigned char)line[i]);
                ++i;
            }
        }
        token[key_len] = '\0';
        token_len = i - start;

        dict_addr = dictionary_lookup(vm, dictionary_addr, token);
        entry_addr = (uint32_t)parse_buffer + 2U +
                     (uint32_t)count * 4U;

        if (!(preserve_unrecognized && dict_addr == 0U)) {
            if (write_word(vm, entry_addr, dict_addr) != TCL_OK ||
                write_byte(vm, entry_addr + 2U,
                           (uint8_t)token_len) != TCL_OK ||
                write_byte(vm, entry_addr + 3U,
                           (uint8_t)(start + text_offset)) != TCL_OK)
                return TCL_ERROR;
        }

        ++count;
    }

    return write_byte(vm, (uint32_t)parse_buffer + 1U, count);
}

/*
 * Consume the Tcl-side pending input and satisfy one Z-machine read request.
 *
 * V1-V4 text buffers reserve byte 0 for maximum size, store text from byte 1,
 * and terminate the text with zero. V5+ retain the maximum in byte 0, store
 * the actual character count in byte 1, and begin text at byte 2. The input is
 * truncated to the story-advertised capacity before it is written.
 */
int zmachine_input_read_line(ZMachine *vm,
                             uint16_t text_buffer,
                             uint16_t parse_buffer,
                             uint16_t *terminator)
{
    const char *line;
    size_t len, i, max_chars;
    uint8_t offset;

    if (!vm || !vm->memory || !vm->input_available)
        return TCL_ERROR;
    if ((size_t)text_buffer >= vm->memory_size)
        return TCL_ERROR;

    line = Tcl_DStringValue(&vm->pending_input);
    len = (size_t)Tcl_DStringLength(&vm->pending_input);
    max_chars = vm->memory[text_buffer];

    if (vm->version <= 4U) {
        /* V1-V4 include room for the terminating zero in the advertised size. */
        if (max_chars > 0U)
            --max_chars;
        offset = 1U;
    } else {
        offset = 2U;
    }

    if (len > max_chars)
        len = max_chars;

    /* Dictionary input is case-insensitive; store a lower-case copy. */
    for (i = 0U; i < len; ++i) {
        unsigned char c =
            (unsigned char)tolower((unsigned char)line[i]);
        if (write_byte(vm, (uint32_t)text_buffer + offset + (uint32_t)i,
                       c) != TCL_OK)
            return TCL_ERROR;
    }

    if (vm->version <= 4U) {
        if (write_byte(vm,
                       (uint32_t)text_buffer + offset + (uint32_t)len,
                       0U) != TCL_OK)
            return TCL_ERROR;
    } else {
        if (write_byte(vm, (uint32_t)text_buffer + 1U,
                       (uint8_t)len) != TCL_OK)
            return TCL_ERROR;
    }

    if (tokenize(vm, parse_buffer, line, len, offset,
                 vm->dictionary_addr, 0) != TCL_OK)
        return TCL_ERROR;

    /* Input has been committed to story memory and can now be discarded. */
    Tcl_DStringSetLength(&vm->pending_input, 0);
    vm->input_available = 0;

    if (terminator)
        *terminator = 13U; /* ZSCII carriage return / Enter key. */

    return TCL_OK;
}

/*
 * Tokenize text which is already stored in a Version 5+ text buffer.
 *
 * Unlike zmachine_input_read_line(), this routine does not touch pending Tcl
 * input. It reads the byte-1 character count and analyzes bytes beginning at
 * byte 2 exactly where V5+ line input stores them.
 */
int zmachine_input_tokenize_buffer(ZMachine *vm,
                                   uint16_t text_buffer,
                                   uint16_t parse_buffer,
                                   uint16_t dictionary,
                                   int preserve_unrecognized)
{
    uint16_t dictionary_addr;
    size_t len;
    uint32_t text_start;

    if (!vm || !vm->memory || vm->version < 5U)
        return TCL_ERROR;
    if ((size_t)text_buffer + 1U >= vm->memory_size)
        return TCL_ERROR;

    len = vm->memory[(uint32_t)text_buffer + 1U];
    text_start = (uint32_t)text_buffer + 2U;
    if ((size_t)text_start + len > vm->memory_size)
        return TCL_ERROR;

    dictionary_addr = dictionary != 0U ? dictionary : vm->dictionary_addr;
    return tokenize(vm, parse_buffer,
                    (const char *)(vm->memory + text_start),
                    len, 2U, dictionary_addr,
                    preserve_unrecognized != 0);
}
