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
 * `tokenise` and `encode_text` opcodes so read-time parsing and explicit
 * dictionary encoding cannot drift. It intentionally contains no terminal,
 * filesystem, or IRC behavior.
 *
 * Story buffers are validated before mutation. This matters for malformed
 * stories: a text buffer crossing static memory or a truncated parse buffer must
 * fail without leaving half of a word, a reset parse count, or part of the new
 * command behind in dynamic memory.
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
 * Return nonzero only when an entire byte range is writable dynamic memory.
 *
 * The subtraction form avoids address+length overflow. Dynamic memory ends at
 * the earlier of the loaded image size and the story's static-memory boundary.
 * A zero-length range is valid at any address up to that boundary.
 */
static int dynamic_range_writable(const ZMachine *vm,
                                  uint32_t address,
                                  size_t length)
{
    size_t start;
    size_t limit;

    if (!vm || !vm->memory)
        return 0;

    start = (size_t)address;
    limit = vm->memory_size;
    if ((size_t)vm->static_memory_addr < limit)
        limit = (size_t)vm->static_memory_addr;

    if (start > limit)
        return 0;
    return length <= limit - start;
}

/* Write one byte to story dynamic memory. */
static int write_byte(ZMachine *vm, uint32_t addr, uint8_t value)
{
    if (!dynamic_range_writable(vm, addr, 1U))
        return TCL_ERROR;

    vm->memory[addr] = value;
    return TCL_OK;
}

/*
 * Write one big-endian word atomically with respect to bounds validation.
 * Validate both destination bytes before changing either one.
 */
static int write_word(ZMachine *vm, uint32_t addr, uint16_t value)
{
    if (!dynamic_range_writable(vm, addr, 2U))
        return TCL_ERROR;

    vm->memory[addr] = (uint8_t)(value >> 8);
    vm->memory[addr + 1U] = (uint8_t)value;
    return TCL_OK;
}

/*
 * Resolve the optional V5+ story-specific alphabet table.
 *
 * Header word $34 is zero for the default alphabets or a byte address of 78
 * ZSCII bytes (26 per alphabet). A malformed nonzero table is an input-encoding
 * error: silently falling back to the defaults would make typed words compare
 * against a different encoding from the story's dictionary.
 */
static int custom_alphabet_address(const ZMachine *vm, uint16_t *address)
{
    uint16_t table;

    if (!vm || !vm->memory || !address)
        return TCL_ERROR;

    *address = 0U;
    if (vm->version < 5U)
        return TCL_OK;
    if (vm->memory_size <= 0x35U)
        return TCL_ERROR;

    table = be16(vm->memory + 0x34U);
    if (table == 0U)
        return TCL_OK;
    if ((size_t)table + 78U > vm->memory_size)
        return TCL_ERROR;

    *address = table;
    return TCL_OK;
}

/*
 * Find one lower-case ZSCII byte in the version's default alphabet table.
 *
 * A0 contains lower-case letters. A1 is not normally needed because dictionary
 * input is lowercased. A2 differs in V1: with no newline entry, digit zero begins
 * at Z-character 7; in V2+ digit zero begins at Z-character 8 because A2/7 is
 * newline. A2/6 is always the 10-bit ZSCII escape and is never a table glyph.
 */
static int default_alphabet_index(uint8_t version,
                                  unsigned char c,
                                  uint8_t *zchar)
{
    const char *p;

    if (c >= 'a' && c <= 'z') {
        *zchar = (uint8_t)(6U + (c - 'a'));
        return 0;
    }

    if (version == 1U) {
        static const char a2_v1[] = "0123456789.,!?_#'\"/\\<-:()";
        p = strchr(a2_v1, (int)c);
        if (p) {
            *zchar = (uint8_t)(7U + (p - a2_v1));
            return 2;
        }
    } else {
        static const char a2_v2plus[] = "0123456789.,!?_#'\"/\\-:()";
        p = strchr(a2_v2plus, (int)c);
        if (p) {
            *zchar = (uint8_t)(8U + (p - a2_v2plus));
            return 2;
        }
    }

    return -1;
}

/*
 * Find one ZSCII byte in the active story alphabet table.
 *
 * Custom tables exist only in V5+. Search A0 first so a directly representable
 * character gets the shortest encoding, then A1 and A2. Entries 6 and 7 of A2
 * remain the escape and newline controls even when bytes are present in those
 * two table positions, so they are deliberately skipped during lookup.
 */
static int alphabet_index(const ZMachine *vm,
                          uint16_t custom_table,
                          unsigned char c,
                          uint8_t *zchar)
{
    int alphabet;

    if (custom_table == 0U)
        return default_alphabet_index(vm->version, c, zchar);

    for (alphabet = 0; alphabet < 3; ++alphabet) {
        unsigned first = alphabet == 2 ? 2U : 0U;
        unsigned i;

        for (i = first; i < 26U; ++i) {
            size_t address = (size_t)custom_table +
                             (size_t)alphabet * 26U + i;
            if (vm->memory[address] == c) {
                *zchar = (uint8_t)(6U + i);
                return alphabet;
            }
        }
    }

    return -1;
}

/* Append as much of a Z-character construction as the fixed key still holds. */
static void append_zchars(uint8_t *zchars,
                          size_t zmax,
                          size_t *used,
                          const uint8_t *sequence,
                          size_t sequence_length)
{
    size_t i;

    for (i = 0U; i < sequence_length && *used < zmax; ++i)
        zchars[(*used)++] = sequence[i];
}

/* Return the V1/V2 temporary or locking shift from one alphabet to another. */
static uint8_t v12_shift_code(int current, int target, int lock)
{
    int delta = (target - current + 3) % 3;
    uint8_t code = delta == 1 ? 2U : 3U;
    return lock ? (uint8_t)(code + 2U) : code;
}

/*
 * Encode bytes into the fixed-width dictionary key format.
 *
 * V1-V3 dictionaries compare six Z-characters (four bytes); V4+ compare nine
 * Z-characters (six bytes). Typed text is lowercased, abbreviations are never
 * used, padding is Z-character 5, and an incomplete shift/escape construction
 * is retained if the fixed dictionary resolution cuts it off.
 *
 * V5+ custom alphabet tables are honored for dictionary encryption just as they
 * are for text decoding. In V1/V2 the persistent alphabet state is tracked so
 * the required shift-lock optimization is used when the current and following
 * input characters both belong to the same non-current alphabet.
 */
static int encode_dictionary_word(const ZMachine *vm,
                                  const uint8_t *word,
                                  size_t word_len,
                                  uint8_t *out,
                                  size_t out_len)
{
    uint8_t zchars[9];
    size_t zmax;
    size_t zi = 0U;
    size_t i;
    uint16_t custom_table;
    int current_alphabet = 0;

    if (!vm || !word || !out)
        return TCL_ERROR;

    zmax = vm->version <= 3U ? 6U : 9U;
    if (out_len < (zmax / 3U) * 2U)
        return TCL_ERROR;
    if (custom_alphabet_address(vm, &custom_table) != TCL_OK)
        return TCL_ERROR;

    memset(zchars, 5, sizeof(zchars));

    for (i = 0U; i < word_len && zi < zmax; ++i) {
        unsigned char c = (unsigned char)tolower((unsigned char)word[i]);
        uint8_t zchar = 0U;
        int alphabet;
        uint8_t sequence[4];
        size_t sequence_length = 0U;

        /* Z-character 0 represents space independently of the alphabet table. */
        if (c == (unsigned char)' ') {
            sequence[sequence_length++] = 0U;
            append_zchars(zchars, zmax, &zi, sequence, sequence_length);
            continue;
        }

        alphabet = alphabet_index(vm, custom_table, c, &zchar);

        if (vm->version <= 2U && alphabet >= 0) {
            if (alphabet != current_alphabet) {
                int lock = 0;

                if (i + 1U < word_len) {
                    unsigned char next =
                        (unsigned char)tolower((unsigned char)word[i + 1U]);
                    uint8_t next_zchar;
                    int next_alphabet = next == (unsigned char)' ' ? -1 :
                        alphabet_index(vm, custom_table, next, &next_zchar);
                    if (next_alphabet == alphabet)
                        lock = 1;
                }

                sequence[sequence_length++] =
                    v12_shift_code(current_alphabet, alphabet, lock);
                if (lock)
                    current_alphabet = alphabet;
            }
            sequence[sequence_length++] = zchar;
        } else if (vm->version <= 2U && alphabet < 0) {
            if (current_alphabet != 2)
                sequence[sequence_length++] =
                    v12_shift_code(current_alphabet, 2, 0);
            sequence[sequence_length++] = 6U;
            sequence[sequence_length++] = (uint8_t)((c >> 5) & 0x1fU);
            sequence[sequence_length++] = (uint8_t)(c & 0x1fU);
        } else if (alphabet == 0) {
            sequence[sequence_length++] = zchar;
        } else if (alphabet == 1) {
            sequence[sequence_length++] = 4U;
            sequence[sequence_length++] = zchar;
        } else if (alphabet == 2) {
            sequence[sequence_length++] = 5U;
            sequence[sequence_length++] = zchar;
        } else {
            sequence[sequence_length++] = 5U;
            sequence[sequence_length++] = 6U;
            sequence[sequence_length++] = (uint8_t)((c >> 5) & 0x1fU);
            sequence[sequence_length++] = (uint8_t)(c & 0x1fU);
        }

        append_zchars(zchars, zmax, &zi, sequence, sequence_length);
    }

    for (i = 0U; i < zmax / 3U; ++i) {
        uint16_t packed =
            (uint16_t)(((uint16_t)zchars[i * 3U] << 10) |
                       ((uint16_t)zchars[i * 3U + 1U] << 5) |
                       zchars[i * 3U + 2U]);
        if (i == zmax / 3U - 1U)
            packed |= 0x8000U;

        out[i * 2U] = (uint8_t)(packed >> 8);
        out[i * 2U + 1U] = (uint8_t)packed;
    }

    return TCL_OK;
}

/*
 * Look up one already-tokenized word in a selected story dictionary.
 *
 * The dictionary header supplies a separator list, entry width, and signed
 * entry count. A negative count indicates an unsorted table. Linear search is
 * valid for both sorted and unsorted dictionaries and is especially useful for
 * `tokenise`, whose optional user dictionary may be modified during play.
 *
 * Malformed dictionary layout is treated as an empty/not-matching dictionary,
 * preserving historical tolerance. A malformed custom alphabet table is
 * different: it makes encryption itself undefined, so that error is propagated
 * to the caller instead of silently using the wrong key.
 */
static int dictionary_lookup(const ZMachine *vm,
                             uint16_t dictionary_addr,
                             const char *word,
                             uint16_t *result)
{
    uint32_t d = dictionary_addr;
    uint8_t separators, entry_len;
    int entry_count;
    uint32_t entries;
    uint8_t encoded[6];
    size_t key_len = vm->version <= 3 ? 4U : 6U;
    int i;

    if (!result)
        return TCL_ERROR;
    *result = 0U;

    if (!vm->memory || d == 0U || d >= vm->memory_size)
        return TCL_OK;

    separators = vm->memory[d++];
    if ((size_t)d + separators > vm->memory_size)
        return TCL_OK;
    d += separators;
    if ((size_t)d + 3U > vm->memory_size)
        return TCL_OK;

    entry_len = vm->memory[d++];
    entry_count = (int16_t)be16(vm->memory + d);
    d += 2U;
    entries = d;

    if (entry_count < 0)
        entry_count = -entry_count;
    if ((size_t)entry_len < key_len)
        return TCL_OK;

    memset(encoded, 0, sizeof(encoded));
    if (encode_dictionary_word(vm,
                               (const uint8_t *)word, strlen(word),
                               encoded, key_len) != TCL_OK)
        return TCL_ERROR;

    for (i = 0; i < entry_count; ++i) {
        uint32_t addr = entries + (uint32_t)i * entry_len;

        if ((size_t)addr + key_len > vm->memory_size)
            break;
        if (memcmp(vm->memory + addr, encoded, key_len) == 0) {
            if (addr > 0xffffU)
                return TCL_OK;
            *result = (uint16_t)addr;
            return TCL_OK;
        }
    }

    return TCL_OK;
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
 * Count parse tokens without mutating the parse buffer.
 *
 * This first pass mirrors only the delimiter rules needed to determine how many
 * four-byte slots the second pass can actually touch. It deliberately limits
 * itself to max_words, just like real tokenization. We do not demand that all
 * max_words advertised slots fit in memory: early Infocom/Inform stories are
 * known to advertise 240 slots in a 240-byte parse array. Validating only the
 * minimum six bytes plus slots needed by this input preserves that historical
 * compatibility while still making every actual write range safe.
 */
static size_t count_parse_tokens(const ZMachine *vm,
                                 uint16_t dictionary_addr,
                                 const char *line,
                                 size_t len,
                                 uint8_t max_words)
{
    size_t i = 0U;
    size_t count = 0U;

    while (i < len && count < max_words) {
        while (i < len && line[i] == ' ')
            ++i;
        if (i >= len)
            break;

        if (is_separator(vm, dictionary_addr, (unsigned char)line[i])) {
            ++i;
        } else {
            while (i < len && line[i] != ' ' &&
                   !is_separator(vm, dictionary_addr,
                                 (unsigned char)line[i]))
                ++i;
        }
        ++count;
    }

    return count;
}

/* Validate a parse buffer before any text or parse-buffer mutation occurs. */
static int validate_parse_buffer(const ZMachine *vm,
                                 uint16_t parse_buffer,
                                 const char *line,
                                 size_t len,
                                 uint16_t dictionary_addr)
{
    uint8_t max_words;
    size_t token_count;
    size_t required;

    if (parse_buffer == 0U)
        return TCL_OK;
    if (!dynamic_range_writable(vm, parse_buffer, 6U))
        return TCL_ERROR;

    max_words = vm->memory[parse_buffer];
    if (max_words == 0U)
        return TCL_ERROR;

    token_count = count_parse_tokens(vm, dictionary_addr,
                                     line, len, max_words);
    required = 2U + token_count * 4U;
    if (required < 6U)
        required = 6U;

    return dynamic_range_writable(vm, parse_buffer, required) ?
           TCL_OK : TCL_ERROR;
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

    if (parse_buffer == 0U)
        return TCL_OK;

    if (validate_parse_buffer(vm, parse_buffer, line, len,
                              dictionary_addr) != TCL_OK)
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

        if (dictionary_lookup(vm, dictionary_addr, token,
                              &dict_addr) != TCL_OK)
            return TCL_ERROR;
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
 *
 * Both text and parse destinations are completely validated before either is
 * modified. On failure, pending Tcl input remains available to the host and the
 * story sees its previous buffer contents unchanged.
 */
int zmachine_input_read_line(ZMachine *vm,
                             uint16_t text_buffer,
                             uint16_t parse_buffer,
                             uint16_t *terminator)
{
    const char *line;
    size_t len, i, max_chars;
    size_t text_required;
    uint8_t offset;
    uint16_t custom_table;

    if (!vm || !vm->memory || !vm->input_available)
        return TCL_ERROR;
    if (!dynamic_range_writable(vm, text_buffer, 1U))
        return TCL_ERROR;
    if (custom_alphabet_address(vm, &custom_table) != TCL_OK)
        return TCL_ERROR;
    (void)custom_table; /* Validation only; lookup repeats it while encoding. */

    line = Tcl_DStringValue(&vm->pending_input);
    len = (size_t)Tcl_DStringLength(&vm->pending_input);
    max_chars = vm->memory[text_buffer];

    if (vm->version <= 4U) {
        if (max_chars < 2U)
            return TCL_ERROR;
        --max_chars;
        offset = 1U;
        if (len > max_chars)
            len = max_chars;
        text_required = 2U + len;
    } else {
        if (max_chars < 1U)
            return TCL_ERROR;
        offset = 2U;
        if (len > max_chars)
            len = max_chars;
        text_required = 2U + len;
    }

    if (text_required < 3U)
        text_required = 3U;
    if (!dynamic_range_writable(vm, text_buffer, text_required))
        return TCL_ERROR;

    if (validate_parse_buffer(vm, parse_buffer, line, len,
                              vm->dictionary_addr) != TCL_OK)
        return TCL_ERROR;

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

    Tcl_DStringSetLength(&vm->pending_input, 0);
    vm->input_available = 0;

    if (terminator)
        *terminator = 13U;

    return TCL_OK;
}

/* Tokenize text already stored in a Version 5+ text buffer. */
int zmachine_input_tokenize_buffer(ZMachine *vm,
                                   uint16_t text_buffer,
                                   uint16_t parse_buffer,
                                   uint16_t dictionary,
                                   int preserve_unrecognized)
{
    uint16_t dictionary_addr;
    uint16_t custom_table;
    size_t len;
    uint32_t text_start;

    if (!vm || !vm->memory || vm->version < 5U)
        return TCL_ERROR;
    if ((size_t)text_buffer + 1U >= vm->memory_size)
        return TCL_ERROR;
    if (custom_alphabet_address(vm, &custom_table) != TCL_OK)
        return TCL_ERROR;
    (void)custom_table;

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

/* Encode an explicit V5+ ZSCII memory slice into a six-byte dictionary key. */
int zmachine_input_encode_text(ZMachine *vm,
                               uint16_t zscii_text,
                               uint16_t length,
                               uint16_t from,
                               uint16_t coded_text)
{
    uint8_t encoded[6];
    uint32_t source;
    size_t i;

    if (!vm || !vm->memory || vm->version < 5U)
        return TCL_ERROR;

    source = (uint32_t)zscii_text + (uint32_t)from;
    if ((size_t)source + length > vm->memory_size)
        return TCL_ERROR;
    if (!dynamic_range_writable(vm, coded_text, sizeof(encoded)))
        return TCL_ERROR;

    if (encode_dictionary_word(vm, vm->memory + source, length,
                               encoded, sizeof(encoded)) != TCL_OK)
        return TCL_ERROR;
    for (i = 0U; i < sizeof(encoded); ++i) {
        if (write_byte(vm, (uint32_t)coded_text + (uint32_t)i,
                       encoded[i]) != TCL_OK)
            return TCL_ERROR;
    }
    return TCL_OK;
}
