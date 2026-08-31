# smoke.tcl
#
# Tcl-facing package smoke test. Besides verifying that the shared library
# loads, this script creates a tiny synthetic V3 session and exercises the
# presentation configuration API used by IRC callers. No real game fixture is
# needed here; the synthetic file only has to contain a valid 64-byte header and
# enough memory for the initial program counter.

if {$argc != 1} {
    error "usage: smoke.tcl /path/to/tclzmachine.so"
}

load [lindex $argv 0] Tclzmachine
package require tclzmachine

if {[package present tclzmachine] ne "0.2.0"} {
    error "unexpected package version"
}

# `zmachine::key` is the explicit numeric ZSCII companion to line-oriented
# `zmachine::command`. Behavioral read_char coverage lives in the C suite; this
# smoke assertion catches an extension build which forgot to register the Tcl API.
if {[namespace which ::zmachine::key] eq ""} {
    error "zmachine::key command was not registered"
}

# Build a minimal 128-byte V3 story image. The VM's loader validates the
# version, initial PC, static-memory base, and basic story length; it does not
# execute this image during the configuration checks below.
set story [string repeat "\x00" 128]
set story [string replace $story 0 0 "\x03"]       ;# version 3
set story [string replace $story 4 5 "\x00\x40"] ;# high memory
set story [string replace $story 6 7 "\x00\x40"] ;# initial PC
set story [string replace $story 8 9 "\x00\x40"] ;# dictionary
set story [string replace $story 10 11 "\x00\x40"] ;# object table
set story [string replace $story 12 13 "\x00\x40"] ;# globals
set story [string replace $story 14 15 "\x00\x40"] ;# static memory

set channel [file tempfile story_path]
fconfigure $channel -translation binary -encoding binary
puts -nonewline $channel $story
close $channel

try {
    if {[zmachine::create smoke $story_path] ne "smoke"} {
        error "unable to create smoke-test session"
    }

    set settings [zmachine::configure smoke]
    if {[dict get $settings -wordwrap] != 0} {
        error "word wrapping should be disabled by default"
    }

    if {[zmachine::configure smoke -wordwrap 400] != 400} {
        error "unable to set word-wrap byte limit"
    }
    if {[zmachine::configure smoke -wordwrap] != 400} {
        error "word-wrap byte limit did not persist"
    }

    if {![catch {zmachine::configure smoke -wordwrap -1} message]} {
        error "negative word-wrap limits must be rejected"
    }
    if {$message ne "-wordwrap must be zero or a positive byte count"} {
        error "unexpected negative word-wrap diagnostic: $message"
    }

    if {[zmachine::configure smoke -wordwrap 0] != 0} {
        error "unable to disable word wrapping"
    }

    zmachine::destroy smoke
} finally {
    catch {zmachine::destroy smoke}
    file delete -force $story_path
}

puts "tclzmachine package and Tcl configuration API loaded successfully"
