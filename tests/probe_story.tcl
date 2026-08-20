# probe_story.tcl
#
# Manual compatibility probe for real Z-machine story files.
#
# This script is intentionally not registered as a CTest test because official
# Infocom story files are copyrighted and must not be committed to this
# repository.  It accepts a locally-owned story file and exercises it through
# the same Tcl extension API an IRC bot will use.
#
# Usage:
#
#   tclsh tests/probe_story.tcl ./build/tclzmachine.so /path/to/story.z3
#   tclsh tests/probe_story.tcl ./build/tclzmachine.so /path/to/story.z3 look
#
# With no player command, the script sends an empty command.  This is useful
# for discovering whether a story reaches its first input request.  Supplying
# a command such as "look" is usually more useful once initial startup works.

if {$argc < 2 || $argc > 3} {
    puts stderr "usage: probe_story.tcl /path/to/tclzmachine.so /path/to/story.zN ?command?"
    exit 2
}

set extension [lindex $argv 0]
set story     [lindex $argv 1]
set command   [expr {$argc == 3 ? [lindex $argv 2] : ""}]

# Load the extension exactly as a production Tcl application would.
if {[catch {load $extension Tclzmachine} err opts]} {
    puts stderr "unable to load tclzmachine extension: $err"
    exit 1
}

if {[catch {package require tclzmachine} err opts]} {
    puts stderr "unable to require tclzmachine package: $err"
    exit 1
}

# Give the probe a fixed session name so repeated shell invocations are easy
# to compare.  Each process owns a fresh interpreter, so there is no collision
# between separate runs of this script.
set session probe

if {[catch {zmachine::create $session $story} err opts]} {
    puts stderr "unable to create story session: $err"
    exit 1
}

puts "Story information:"
puts [zmachine::info $session]
puts ""
puts "Command: [list $command]"
puts "--- output begins ---"

# The command API runs until the story halts, errors, or asks for another line
# of input.  Any VM diagnostic is deliberately printed together with the
# session information so unsupported opcodes can be implemented from the
# precise failure point rather than guessed at.
if {[catch {zmachine::command $session $command} result opts]} {
    puts stderr "--- VM ERROR ---"
    puts stderr $result
    puts stderr "Session information after failure:"
    if {![catch {zmachine::info $session} info]} {
        puts stderr $info
    }
    zmachine::destroy $session
    exit 1
}

puts -nonewline $result
if {$result eq "" || ![string match *\n $result]} {
    puts ""
}
puts "--- output ends ---"
puts ""
puts "Session information after command:"
puts [zmachine::info $session]

zmachine::destroy $session
