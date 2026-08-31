# integration_v3.tcl
#
# End-to-end Tcl regression using the repository-owned no-library V3 story.
# The fixture is intentionally small, but it crosses the public Tcl API and
# exercises the Version 3 input-buffer format, routine calls, globals, object
# relationships, insert_obj, branches, Quetzal branch-form save/restore, and
# persistent state across turns.

if {$argc != 2} {
    error "usage: integration_v3.tcl /path/to/tclzmachine.so /path/to/story.z3"
}

set extension [lindex $argv 0]
set story     [lindex $argv 1]
set savefile  [file normalize [file join [pwd] "tclzmachine-v3-[pid].sav"]]

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

proc complete_file_request {session operation path} {
    if {[catch {zmachine::$operation $session $path} result]} {
        set info "unavailable"
        catch {set info [zmachine::info $session]}
        error "$operation failed for [list $path]: $result\nSession information: $info"
    }
    return $result
}

load $extension Tclzmachine
package require tclzmachine

if {![file exists $story]} {
    error "V3 integration story does not exist: $story"
}

catch {file delete -force $savefile}
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

    # Establish the state which the V3 save branch must preserve.
    set output [run_command integration_v3 "south"]
    require_contains "south before save" $output "V3 Test Chamber"

    set output [run_command integration_v3 "save"]
    set info [zmachine::info integration_v3]
    if {[dict get $info fileRequest] ne "save"} {
        error "V3 save did not yield a save file request: $info\n$output"
    }

    set output [complete_file_request integration_v3 save $savefile]
    require_contains "save completion" $output "Save completed."
    if {![file exists $savefile]} {
        error "V3 save did not create Quetzal file: $savefile"
    }

    # Diverge both object and room state after the saved point.
    set output [run_command integration_v3 "drop lamp"]
    require_contains "drop after save" $output "Dropped."

    set output [run_command integration_v3 "north"]
    require_contains "north after save" $output "V3 North Room"

    set output [run_command integration_v3 "inventory"]
    require_contains "mutated inventory" $output "You are carrying nothing."

    # V3 restore never branches at the restore opcode on success. It reloads the
    # old save continuation and takes that save opcode's success branch again.
    set output [run_command integration_v3 "restore"]
    set info [zmachine::info integration_v3]
    if {[dict get $info fileRequest] ne "restore"} {
        error "V3 restore did not yield a restore file request: $info\n$output"
    }

    set output [complete_file_request integration_v3 restore $savefile]
    require_contains "restore completion" $output "Save completed."

    # Both dynamic-memory global state and V3 object-tree state must roll back.
    set output [run_command integration_v3 "look"]
    require_contains "restored location" $output "V3 Test Chamber"

    set output [run_command integration_v3 "inventory"]
    require_contains "restored inventory" $output "You are carrying the brass lamp."

    set info [zmachine::info integration_v3]
    if {[dict get $info fileRequest] ne ""} {
        error "unexpected pending file request after V3 restore: $info"
    }
} finally {
    catch {zmachine::destroy integration_v3}
    catch {file delete -force $savefile}
}

puts "repository-owned V3 integration story passed scripted session and Quetzal restore"
