# probe_catalog.tcl
#
# Batch compatibility probe for a directory of locally-owned Z-machine story
# files.  Official Infocom stories are copyrighted and are intentionally never
# committed to this repository; this script lets a developer exercise a local
# collection through the same Tcl API an IRC bot will use.
#
# Usage:
#
#   tclsh tests/probe_catalog.tcl ./build/tclzmachine.so /path/to/stories
#   tclsh tests/probe_catalog.tcl ./build/tclzmachine.so /path/to/stories look
#
# The optional command defaults to "look".  Each story runs in its own fresh
# session so a crash/error in one game cannot contaminate another VM instance.
# The script prints a compact summary first and preserves the exact VM error for
# failed stories so missing-opcode work can be prioritized by real usage.

if {$argc < 2 || $argc > 3} {
    puts stderr "usage: probe_catalog.tcl /path/to/tclzmachine.so /path/to/story-directory ?command?"
    exit 2
}

set extension [file normalize [lindex $argv 0]]
set directory [file normalize [lindex $argv 1]]
set command   [expr {$argc == 3 ? [lindex $argv 2] : "look"}]

if {![file isdirectory $directory]} {
    puts stderr "story directory does not exist: $directory"
    exit 2
}

if {[catch {load $extension Tclzmachine} err]} {
    puts stderr "unable to load tclzmachine extension: $err"
    exit 1
}

if {[catch {package require tclzmachine} err]} {
    puts stderr "unable to require tclzmachine package: $err"
    exit 1
}

# Infocom collections commonly use .dat as well as explicit .zN suffixes.
set stories {}
foreach pattern {*.dat *.z1 *.z2 *.z3 *.z4 *.z5 *.z6 *.z7 *.z8} {
    foreach path [glob -nocomplain -directory $directory $pattern] {
        if {[file isfile $path]} {
            lappend stories [file normalize $path]
        }
    }
}
set stories [lsort -dictionary -unique $stories]

if {[llength $stories] == 0} {
    puts stderr "no .dat or .z1-.z8 story files found in $directory"
    exit 2
}

set passed 0
set failed 0
set skipped 0
set failures {}

puts "tclzmachine catalog compatibility probe"
puts "Directory: $directory"
puts "Command: [list $command]"
puts "Stories: [llength $stories]"
puts ""

set index 0
foreach story $stories {
    incr index
    set session "catalog_$index"
    set name [file tail $story]

    # Creating the session validates the Z-machine version/header before any
    # bytecode executes.  Version 6 is an intentional unsupported case rather
    # than a compatibility regression, so report it separately as SKIP.
    if {[catch {zmachine::create $session $story} err]} {
        if {[string match {*Version 6*intentionally unsupported*} $err]} {
            incr skipped
            puts [format "SKIP  %-32s %s" $name $err]
        } else {
            incr failed
            puts [format "FAIL  %-32s create: %s" $name $err]
            lappend failures [list $name "create: $err"]
        }
        continue
    }

    set before [zmachine::info $session]
    if {[catch {zmachine::command $session $command} output]} {
        incr failed
        set after [zmachine::info $session]
        puts [format "FAIL  %-32s %s" $name $output]
        lappend failures [list $name $output $after]
        zmachine::destroy $session
        continue
    }

    incr passed
    set after [zmachine::info $session]
    set version [dict get $before version]
    set state [dict get $after state]
    set bytes [string bytelength $output]
    puts [format "PASS  %-32s V%-1s output=%-6d state=%s" \
              $name $version $bytes $state]

    zmachine::destroy $session
}

puts ""
puts "Summary: PASS=$passed FAIL=$failed SKIP=$skipped TOTAL=[llength $stories]"

if {[llength $failures] > 0} {
    puts ""
    puts "Failure details:"
    foreach failure $failures {
        puts "------------------------------------------------------------"
        puts "Story: [lindex $failure 0]"
        puts "Error: [lindex $failure 1]"
        if {[llength $failure] > 2} {
            puts "State: [lindex $failure 2]"
        }
    }
}

# A nonzero exit makes this useful in ad-hoc compatibility scripts while still
# treating intentionally unsupported V6 stories as skips rather than failures.
exit [expr {$failed == 0 ? 0 : 1}]
