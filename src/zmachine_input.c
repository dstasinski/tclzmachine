#include "tclzmachine.h"
#include "zmachine_input.h"

#include <ctype.h>
#include <string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int write_byte(ZMachine *vm, uint32_t addr, uint8_t value)
{
    if (!vm || !vm->memory || addr >= vm->memory_size || addr >= vm->static_memory_addr)
        return TCL_ERROR;
    vm->memory[addr] = value;
    return TCL_OK;
}

static int write_word(ZMachine *vm, uint32_t addr, uint16_t value)
{
    if (write_byte(vm, addr, (uint8_t)(value >> 8)) != TCL_OK)
        return TCL_ERROR;
    return write_byte(vm, addr + 1U, (uint8_t)value);
}

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

static void encode_dictionary_word(uint8_t version, const char *word,
                                   uint8_t *out, size_t out_len)
{
    uint8_t zchars[9];
    size_t zmax = version <= 3 ? 6U : 9U;
    size_t zi = 0U, i;
    uint16_t words[3] = {0,0,0};

    memset(zchars, 5, sizeof(zchars));

    for (i = 0U; word[i] && zi < zmax; ++i) {
        unsigned char c = (unsigned char)tolower((unsigned char)word[i]);
        uint8_t z;
        int alphabet = alphabet_index(c, &z);

        if (alphabet == 0) {
            zchars[zi++] = z;
        } else if (alphabet == 2 && zi + 1U < zmax) {
            zchars[zi++] = (version <= 2) ? 3U : 5U;
            zchars[zi++] = z;
        } else if (zi + 3U < zmax) {
            zchars[zi++] = (version <= 2) ? 3U : 5U;
            zchars[zi++] = 6U;
            zchars[zi++] = (uint8_t)((c >> 5) & 0x1fU);
            zchars[zi++] = (uint8_t)(c & 0x1fU);
        }
    }

    for (i = 0U; i < zmax / 3U; ++i) {
        words[i] = (uint16_t)(((uint16_t)zchars[i*3U] << 10) |
                              ((uint16_t)zchars[i*3U+1U] << 5) |
                              zchars[i*3U+2U]);
        if (i == zmax / 3U - 1U)
            words[i] |= 0x8000U;
        out[i*2U] = (uint8_t)(words[i] >> 8);
        out[i*2U+1U] = (uint8_t)words[i];
    }
    (void)out_len;
}

static uint16_t dictionary_lookup(const ZMachine *vm, const char *word)
{
    uint32_t d = vm->dictionary_addr;
    uint8_t separators, entry_len;
    int16_t entry_count;
    uint32_t entries;
    uint8_t encoded[6];
    size_t key_len = vm->version <= 3 ? 4U : 6U;
    int i;

    if (!vm->memory || d >= vm->memory_size)
        return 0U;

    separators = vm->memory[d++];
    d += separators;
    if (d + 3U > vm->memory_size)
        return 0U;

    entry_len = vm->memory[d++];
    entry_count = (int16_t)be16(vm->memory + d);
    d += 2U;
    entries = d;
    if (entry_count < 0)
        entry_count = (int16_t)-entry_count;

    memset(encoded, 0, sizeof(encoded));
    encode_dictionary_word(vm->version, word, encoded, key_len);

    for (i = 0; i < entry_count; ++i) {
        uint32_t addr = entries + (uint32_t)i * entry_len;
        if (addr + key_len > vm->memory_size)
            break;
        if (memcmp(vm->memory + addr, encoded, key_len) == 0)
            return (uint16_t)addr;
    }

    return 0U;
}

static int is_separator(const ZMachine *vm, unsigned char c)
{
    uint32_t d = vm->dictionary_addr;
    uint8_t count, i;

    if (!vm->memory || d >= vm->memory_size)
        return 0;
    count = vm->memory[d++];
    for (i = 0U; i < count && d + i < vm->memory_size; ++i)
        if (vm->memory[d + i] == c)
            return 1;
    return 0;
}

static int tokenize(ZMachine *vm, uint16_t parse_buffer,
                    const char *line, size_t len, uint8_t text_offset)
{
    uint8_t max_words, count = 0U;
    size_t i = 0U;

    if (parse_buffer == 0U)
        return TCL_OK;
    if ((size_t)parse_buffer + 1U >= vm->memory_size)
        return TCL_ERROR;

    max_words = vm->memory[parse_buffer];
    if (write_byte(vm, (uint32_t)parse_buffer + 1U, 0U) != TCL_OK)
        return TCL_ERROR;

    while (i < len && count < max_words) {
        char token[128];
        size_t start, tlen = 0U;
        uint16_t dict_addr;
        uint32_t entry_addr;

        while (i < len && line[i] == ' ')
            ++i;
        if (i >= len)
            break;

        start = i;
        if (is_separator(vm, (unsigned char)line[i])) {
            token[tlen++] = line[i++];
        } else {
            while (i < len && line[i] != ' ' &&
                   !is_separator(vm, (unsigned char)line[i])) {
                if (tlen + 1U < sizeof(token))
                    token[tlen++] = (char)tolower((unsigned char)line[i]);
                ++i;
            }
        }
        token[tlen] = '\0';
        dict_addr = dictionary_lookup(vm, token);

        entry_addr = (uint32_t)parse_buffer + 2U + (uint32_t)count * 4U;
        if (write_word(vm, entry_addr, dict_addr) != TCL_OK ||
            write_byte(vm, entry_addr + 2U, (uint8_t)tlen) != TCL_OK ||
            write_byte(vm, entry_addr + 3U, (uint8_t)(start + text_offset)) != TCL_OK)
            return TCL_ERROR;
        ++count;
    }

    return write_byte(vm, (uint32_t)parse_buffer + 1U, count);
}

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
        if (max_chars > 0U)
            --max_chars;
        offset = 1U;
    } else {
        offset = 2U;
    }
    if (len > max_chars)
        len = max_chars;

    for (i = 0U; i < len; ++i) {
        unsigned char c = (unsigned char)tolower((unsigned char)line[i]);
        if (write_byte(vm, (uint32_t)text_buffer + offset + (uint32_t)i, c) != TCL_OK)
            return TCL_ERROR;
    }

    if (vm->version <= 4U) {
        if (write_byte(vm, (uint32_t)text_buffer + offset + (uint32_t)len, 0U) != TCL_OK)
            return TCL_ERROR;
    } else {
        if (write_byte(vm, (uint32_t)text_buffer + 1U, (uint8_t)len) != TCL_OK)
            return TCL_ERROR;
    }

    if (tokenize(vm, parse_buffer, line, len, offset) != TCL_OK)
        return TCL_ERROR;

    Tcl_DStringSetLength(&vm->pending_input, 0);
    vm->input_available = 0;
    if (terminator)
        *terminator = 13U;
    return TCL_OK;
}
