# integration.tcl
#
# End-to-end Tcl regression using the repository-owned compiled V5 story.
#
# Unlike the focused synthetic C tests, this script runs a real Inform-built
# story through the public Tcl extension API. It checks parser-driven command
# handling, persistent world state across multiple calls, and a complete
# cooperative Quetzal save/restore cycle through the same API an IRC bot uses.

if {$argc != 2} {
    error "usage: integration.tcl /path/to/tclzmachine.so /path/to/story.z5"
}

set extension [lindex $argv 0]
set story     [lindex $argv 1]
set savefile  [file normalize [file join [pwd] "tclzmachine-integration-[pid].sav"]]

proc require_contains {label text needle} {
    if {[string first $needle $text] < 0} {
        error "$label output did not contain [list $needle]:\n$text"
    }
}

proc require_not_contains {label text needle} {
    if {[string first $needle $text] >= 0} {
        error "$label output unexpectedly contained [list $needle]:\n$text"
    }
}

# Execute one player turn while preserving the VM's diagnostic location. The
# Tcl extension intentionally returns the concise VM error string, so this test
# adds zmachine::info when a real-story regression fails to make the exact PC
# and run state visible in CTest/GitHub Actions output.
proc run_command {session command} {
    if {[catch {zmachine::command $session $command} result]} {
        set info "unavailable"
        catch {set info [zmachine::info $session]}
        error "command [list $command] failed: $result\nSession information: $info"
    }
    return $result
}

# Complete one cooperative file request and report the current VM state if the
# persistence operation fails. A successful completion runs the restored/saved
# story onward until its next cooperative boundary, just like command does.
proc complete_file_request {operation session path} {
    if {[catch {zmachine::$operation $session $path} result]} {
        set info "unavailable"
        catch {set info [zmachine::info $session]}
        error "$operation [list $path] failed: $result\nSession information: $info"
    }
    return $result
}

load $extension Tclzmachine
package require tclzmachine

if {![file exists $story]} {
    error "integration story does not exist: $story"
}

catch {file delete -force $savefile}
zmachine::create integration $story
try {
    set info [zmachine::info integration]
    if {[dict get $info version] != 5} {
        error "integration fixture is not Z-machine Version 5: $info"
    }
    if {[dict get $info textOnly] != 1} {
        error "integration session unexpectedly lost text-only mode"
    }

    # The first command also boots the story. Require only project-owned text,
    # not the Inform library's surrounding banner/prompt formatting.
    set output [run_command integration "look"]
    require_contains "look" $output "tclzmachine integration fixture ready."
    require_contains "look" $output "You are in a small test laboratory."

    # Move a real object through the Inform parser and verify that the object
    # remains in inventory on the following, separate Tcl call.
    set output [run_command integration "take lamp"]
    require_contains "take lamp" $output "Taken."

    set output [run_command integration "inventory"]
    require_contains "inventory" $output "brass lamp"

    # Room movement exercises parser routines, object properties, branches,
    # calls/returns, and persistent location state in the VM.
    set output [run_command integration "north"]
    require_contains "north" $output "North Room"
    require_contains "north" $output "This is the north room."

    set output [run_command integration "south"]
    require_contains "south" $output "Test Lab"
    require_contains "south" $output "You are in a small test laboratory."

    # Inventory must still contain the lamp after two room transitions.
    set output [run_command integration "inventory"]
    require_contains "pre-save inventory" $output "brass lamp"

    #
    # Drive a genuine Inform Save action into the interpreter's cooperative
    # filename boundary. The command call must stop with fileRequest=save; Tcl
    # then chooses the host pathname and resumes the story by completing it.
    #
    set output [run_command integration "save"]
    set info [zmachine::info integration]
    if {[dict get $info fileRequest] ne "save"} {
        error "Inform save did not produce a cooperative save request: $info\n$output"
    }

    set output [complete_file_request save integration $savefile]
    if {![file exists $savefile] || [file size $savefile] <= 0} {
        error "Quetzal save file was not created or was empty: $savefile"
    }
    set info [zmachine::info integration]
    if {[dict get $info fileRequest] ne ""} {
        error "save request remained pending after completion: $info"
    }

    # Deliberately diverge from the saved world: drop the lamp and move north.
    # Subsequent restore checks must recover both object ownership and location.
    run_command integration "drop lamp"
    set output [run_command integration "inventory"]
    require_not_contains "post-save inventory mutation" $output "brass lamp"

    set output [run_command integration "north"]
    require_contains "post-save movement" $output "North Room"

    # A genuine Inform Restore action should yield exactly the complementary
    # host-file request. Completing it reloads the saved frames/memory and
    # resumes the original save opcode with the Version 5 restored result (2).
    set output [run_command integration "restore"]
    set info [zmachine::info integration]
    if {[dict get $info fileRequest] ne "restore"} {
        error "Inform restore did not produce a cooperative restore request: $info\n$output"
    }

    complete_file_request restore integration $savefile
    set info [zmachine::info integration]
    if {[dict get $info fileRequest] ne ""} {
        error "restore request remained pending after completion: $info"
    }

    # The saved point was in Test Lab while carrying the lamp. Query the story
    # normally after restore rather than depending on library-specific save
    # success text emitted while the restored call stack unwinds.
    set output [run_command integration "look"]
    require_contains "restored location" $output "Test Lab"
    require_contains "restored location" $output "You are in a small test laboratory."

    set output [run_command integration "inventory"]
    require_contains "restored inventory" $output "brass lamp"
} finally {
    catch {zmachine::destroy integration}
    catch {file delete -force $savefile}
}

puts "repository-owned V5 integration story passed play and Quetzal persistence"
