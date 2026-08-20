# probe_session.tcl
#
# Manual multi-turn compatibility probe for locally-owned Z-machine stories.
#
# Unlike probe_story.tcl, this script keeps one Tcl Z-machine session alive
# across several commands.  That exercises the same persistent-session model
# expected by an IRC bot: story memory, routine state, objects, globals, and
# other VM state remain associated with the same session between commands.
#
# Official Infocom story files are intentionally not committed to this
# repository.  Point this script at a story file you legally own.
#
# Usage:
#
#   tclsh tests/probe_session.tcl ./build/tclzmachine.so story.z3 \
#       "look" "open mailbox" "read leaflet"
#
# Each command is printed before its returned response.  On a VM error, the
# session information is emitted so an unsupported opcode can be identified
# from the exact program counter and run state.

if {$argc < 3} {
    puts stderr "usage: probe_session.tcl /path/to/tclzmachine.so /path/to/story.zN command ?command ...?"
    exit 2
}

set extension [lindex $argv 0]
set story     [lindex $argv 1]
set commands  [lrange $argv 2 end]

if {[catch {load $extension Tclzmachine} err]} {
    puts stderr "unable to load tclzmachine extension: $err"
    exit 1
}

if {[catch {package require tclzmachine} err]} {
    puts stderr "unable to require tclzmachine package: $err"
    exit 1
}

set session probe

if {[catch {zmachine::create $session $story} err]} {
    puts stderr "unable to create story session: $err"
    exit 1
}

puts "Initial story information:"
puts [zmachine::info $session]
puts ""

set turn 0
foreach command $commands {
    incr turn
    puts "===== turn $turn ====="
    puts "Command: [list $command]"
    puts "--- output begins ---"

    if {[catch {zmachine::command $session $command} result]} {
        puts stderr "--- VM ERROR ON TURN $turn ---"
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
    puts "State: [zmachine::info $session]"
    puts ""
}

zmachine::destroy $session
