# smoke.tcl
#
# Tcl-facing package smoke test. Besides verifying that the shared library
# loads, this script creates tiny synthetic V3/V5 sessions and exercises the
# presentation configuration plus cooperative input/host-stream metadata used by
# embedding applications. No external game fixture is needed here.

if {$argc != 1} {
    error "usage: smoke.tcl /path/to/tclzmachine.so"
}

load [lindex $argv 0] Tclzmachine
package require tclzmachine

if {[package present tclzmachine] ne "0.2.0"} {
    error "unexpected package version"
}

# Explicit line/key input and host-file streams are distinct Tcl APIs. These
# assertions catch a shared-library build which omitted either command even when
# lower-level C regressions still link successfully.
if {[namespace which ::zmachine::key] eq ""} {
    error "zmachine::key command was not registered"
}
if {[namespace which ::zmachine::streamfile] eq ""} {
    error "zmachine::streamfile command was not registered"
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

    set info [zmachine::info smoke]
    foreach key {streamRequest inputStream commandRecording} {
        if {![dict exists $info $key]} {
            error "zmachine::info is missing stream metadata key $key"
        }
    }
    if {[dict get $info streamRequest] ne "" ||
        [dict get $info inputStream] != 0 ||
        [dict get $info commandRecording]} {
        error "unexpected default external stream state: $info"
    }

    zmachine::destroy smoke
} finally {
    catch {zmachine::destroy smoke}
    file delete -force $story_path
}

#
# Build a synthetic V5 loop:
#
#   $40: read      $80 -> g16
#   $45: read_char 1   -> g17
#   $49: jump      $40
#
# A normal command satisfies the first line read and leaves the VM suspended on
# read_char. Supplying an exact key then loops back to a line read with no input
# queued. This exercises both inputRequest values through the real Tcl API.
#
set input_story [string repeat "\x00" 256]
set input_story [string replace $input_story 0 0 "\x05"]
set input_story [string replace $input_story 4 5 "\x00\xC0"] ;# high memory
set input_story [string replace $input_story 6 7 "\x00\x40"] ;# initial PC
set input_story [string replace $input_story 8 9 "\x00\xA0"] ;# dictionary
set input_story [string replace $input_story 10 11 "\x00\xA0"] ;# object table
set input_story [string replace $input_story 12 13 "\x00\x90"] ;# globals
set input_story [string replace $input_story 14 15 "\x00\xC0"] ;# static memory
set input_story [string replace $input_story 64 75 \
    "\xE4\x3F\x00\x80\x10\xF6\x7F\x01\x11\x8C\xFF\xF6"]
set input_story [string replace $input_story 128 128 "\x14"] ;# text max 20

set input_channel [file tempfile input_story_path]
fconfigure $input_channel -translation binary -encoding binary
puts -nonewline $input_channel $input_story
close $input_channel

try {
    if {[zmachine::create inputmeta $input_story_path] ne "inputmeta"} {
        error "unable to create input-metadata session"
    }

    if {[dict get [zmachine::info inputmeta] inputRequest] ne ""} {
        error "a ready session must not advertise a pending input request"
    }

    zmachine::command inputmeta "x"
    set info [zmachine::info inputmeta]
    if {[dict get $info inputRequest] ne "char"} {
        error "expected read_char input request, got: [dict get $info inputRequest]"
    }

    zmachine::key inputmeta 129
    set info [zmachine::info inputmeta]
    if {[dict get $info inputRequest] ne "line"} {
        error "expected line input request after key, got: [dict get $info inputRequest]"
    }

    zmachine::destroy inputmeta
} finally {
    catch {zmachine::destroy inputmeta}
    file delete -force $input_story_path
}

puts "tclzmachine package, configuration, input, and stream metadata APIs loaded successfully"
