# integration_v3.tcl
#
# End-to-end Tcl regression using the repository-owned no-library V3 story.
# The fixture is intentionally small, but it crosses the public Tcl API and
# exercises the Version 3 input-buffer format, routine calls, globals, object
# relationships, insert_obj, branches, and persistent state across turns.

if {$argc != 2} {
    error "usage: integration_v3.tcl /path/to/tclzmachine.so /path/to/story.z3"
}

set extension [lindex $argv 0]
set story     [lindex $argv 1]

proc require_contains {label text needle} {
    if {[string first $needle $text] < 0} {
        error "$label output did not contain [list $needle]:\n$text"
    }
}

proc run_command {session command} {
    if {[catch {zmachine::command $session $command} result]} {
        set info "unavailable"
        catch {set info [zmachine::info $session]}
        error "command [list $command] failed: $result\nSession information: $info"
    }
    return $result
}

load $extension Tclzmachine
package require tclzmachine

if {![file exists $story]} {
    error "V3 integration story does not exist: $story"
}

zmachine::create integration_v3 $story
try {
    set info [zmachine::info integration_v3]
    if {[dict get $info version] != 3} {
        error "integration fixture is not Z-machine Version 3: $info"
    }

    # The first Tcl command boots the story and is consumed by its first sread.
    set output [run_command integration_v3 "look"]
    require_contains "initial look" $output "tclzmachine V3 fixture ready."
    require_contains "initial look" $output "V3 object tree ready."
    require_contains "initial look" $output "V3 Test Chamber"

    # Moving the lamp exercises V1-V3 object fields and insert_obj.
    set output [run_command integration_v3 "take lamp"]
    require_contains "take lamp" $output "Taken."

    set output [run_command integration_v3 "inventory"]
    require_contains "inventory" $output "You are carrying the brass lamp."

    # Room state is held in a global and must persist across separate Tcl calls.
    set output [run_command integration_v3 "north"]
    require_contains "north" $output "V3 North Room"

    set output [run_command integration_v3 "look"]
    require_contains "north look" $output "V3 North Room"

    set output [run_command integration_v3 "south"]
    require_contains "south" $output "V3 Test Chamber"

    # The object move must survive room transitions.
    set output [run_command integration_v3 "inventory"]
    require_contains "final inventory" $output "You are carrying the brass lamp."

    set info [zmachine::info integration_v3]
    if {[dict get $info fileRequest] ne ""} {
        error "unexpected pending file request in V3 fixture: $info"
    }
} finally {
    catch {zmachine::destroy integration_v3}
}

puts "repository-owned V3 integration story passed scripted session"
