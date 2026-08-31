# integration.tcl
#
# End-to-end Tcl regression using the repository-owned compiled V5 story.
#
# Unlike the focused synthetic C tests, this script runs a real Inform-built
# story through the public Tcl extension API.  It checks parser-driven command
# handling and verifies that object/location state persists across multiple
# zmachine::command calls, which is the same resident-session model expected by
# an IRC bot.

if {$argc != 2} {
    error "usage: integration.tcl /path/to/tclzmachine.so /path/to/story.z5"
}

set extension [lindex $argv 0]
set story     [lindex $argv 1]

proc require_contains {label text needle} {
    if {[string first $needle $text] < 0} {
        error "$label output did not contain [list $needle]:\n$text"
    }
}

load $extension Tclzmachine
package require tclzmachine

if {![file exists $story]} {
    error "integration story does not exist: $story"
}

zmachine::create integration $story
try {
    set info [zmachine::info integration]
    if {[dict get $info version] != 5} {
        error "integration fixture is not Z-machine Version 5: $info"
    }
    if {[dict get $info textOnly] != 1} {
        error "integration session unexpectedly lost text-only mode"
    }

    # The first command also boots the story.  Require only project-owned text,
    # not the Inform library's surrounding banner/prompt formatting.
    set output [zmachine::command integration "look"]
    require_contains "look" $output "tclzmachine integration fixture ready."
    require_contains "look" $output "You are in a small test laboratory."

    # Move a real object through the Inform parser and verify that the object
    # remains in inventory on the following, separate Tcl call.
    set output [zmachine::command integration "take lamp"]
    require_contains "take lamp" $output "Taken."

    set output [zmachine::command integration "inventory"]
    require_contains "inventory" $output "brass lamp"

    # Room movement exercises parser routines, object properties, branches,
    # calls/returns, and persistent location state in the VM.
    set output [zmachine::command integration "north"]
    require_contains "north" $output "North Room"
    require_contains "north" $output "This is the north room."

    set output [zmachine::command integration "south"]
    require_contains "south" $output "Test Lab"
    require_contains "south" $output "You are in a small test laboratory."

    # Inventory must still contain the lamp after two room transitions.
    set output [zmachine::command integration "inventory"]
    require_contains "final inventory" $output "brass lamp"

    set info [zmachine::info integration]
    if {[dict get $info fileRequest] ne ""} {
        error "unexpected pending file request after ordinary play: $info"
    }
} finally {
    catch {zmachine::destroy integration}
}

puts "repository-owned V5 integration story passed scripted session"
