/*
 * wrap.c
 *
 * Regression tests for the presentation-layer word wrapper used by Tcl/IRC
 * callers.  These tests deliberately operate without a story file because
 * wrapping must remain independent from Z-machine execution state.
 */

#include "zmachine_wrap.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Compare a Tcl_DString against the exact byte sequence expected. */
static void assert_string(const Tcl_DString *value, const char *expected)
{
    assert((size_t)Tcl_DStringLength((Tcl_DString *)value) == strlen(expected));
    assert(memcmp(Tcl_DStringValue((Tcl_DString *)value),
                  expected, strlen(expected)) == 0);
}

int main(void)
{
    Tcl_DString out;
    Tcl_DStringInit(&out);

    /* A zero limit is the documented no-op/default behavior. */
    assert(zmachine_wrap_output("alpha beta", 10U, 0U, &out) == TCL_OK);
    assert_string(&out, "alpha beta");

    /* Prefer whitespace and omit the whitespace consumed at a new line. */
    assert(zmachine_wrap_output("alpha beta gamma", 16U, 10U, &out) == TCL_OK);
    assert_string(&out, "alpha beta\ngamma");

    /* Existing story newlines remain authoritative paragraph boundaries. */
    assert(zmachine_wrap_output("one two\nthree four", 18U, 5U, &out) == TCL_OK);
    assert_string(&out, "one\ntwo\nthree\nfour");

    /*
     * Long UTF-8 words may require a hard byte split, but never inside the
     * two-byte UTF-8 encoding of U+00E9 (c3 a9).  The configured limit here
     * is four bytes, so the five-byte sequence "écaf" cannot fit on one line;
     * the correct byte-safe wrapping is "caf", "éca", then "fé".
     *
     * Adjacent C string literals terminate the \x escapes before the following
     * hexadecimal-looking ASCII characters can be consumed by the compiler.
     */
    {
        static const char utf8_word[] = "caf\xC3\xA9" "caf\xC3\xA9";
        static const char expected[] = "caf\n\xC3\xA9" "ca\nf\xC3\xA9";
        assert(zmachine_wrap_output(utf8_word, sizeof(utf8_word) - 1U,
                                    4U, &out) == TCL_OK);
        assert_string(&out, expected);
    }

    Tcl_DStringFree(&out);
    puts("output wrapping tests passed");
    return 0;
}
