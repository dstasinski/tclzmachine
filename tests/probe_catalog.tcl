# probe_catalog.tcl
#
# Batch compatibility probe for a directory of locally-owned Z-machine story
# files. Official Infocom stories are copyrighted and are intentionally never
# committed to this repository; this script lets a developer exercise a local
# collection through the same Tcl API an IRC bot will use.
#
# Usage:
#
#   tclsh tests/probe_catalog.tcl ./build/tclzmachine.so /path/to/stories
#   tclsh tests/probe_catalog.tcl ./build/tclzmachine.so /path/to/stories look
#
# The optional command defaults to "look". Each story runs in its own fresh
# session so a crash/error in one game cannot contaminate another VM instance.
# The script prints a compact summary first and preserves the exact VM/Tcl error
# for failed stories so missing-opcode work can be prioritized by real usage.
# A short hexadecimal window around a failing program counter is also reported;
# this is read from the developer's local story file and is never stored in the
# repository.

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

# Read the first story byte, which is always the Z-machine version number.
# Collections often contain unrelated *.dat resource files alongside genuine
# Infocom *.dat stories, so extension alone is not enough to classify a file.
# Return -1 only when the file itself cannot be read.
proc story_header_version {story} {
    if {[catch {
        set channel [open $story rb]
        fconfigure $channel -translation binary -encoding binary
        set data [read $channel 1]
        close $channel
    }]} {
        catch {close $channel}
        return -1
    }

    if {[string length $data] != 1} {
        return -1
    }
    binary scan $data c byte
    return [expr {$byte & 0xff}]
}

# Return a compact hexadecimal window around a failing story PC. The marker
# >xx< identifies the byte at the reported PC. This intentionally reads only a
# few bytes from the local story and never copies story material into fixtures.
proc story_pc_hex_window {story pc} {
    if {![string is integer -strict $pc] || $pc < 0} {
        return ""
    }

    set start [expr {$pc >= 8 ? $pc - 8 : 0}]
    set count 24
    if {[catch {
        set channel [open $story rb]
        fconfigure $channel -translation binary -encoding binary
        seek $channel $start start
        set data [read $channel $count]
        close $channel
    }]} {
        catch {close $channel}
        return ""
    }

    binary scan $data c* bytes
    set rendered {}
    set offset 0
    foreach byte $bytes {
        set value [expr {$byte & 0xff}]
        if {$start + $offset == $pc} {
            lappend rendered [format ">%02x<" $value]
        } else {
            lappend rendered [format "%02x" $value]
        }
        incr offset
    }

    if {[llength $rendered] == 0} {
        return ""
    }
    return [format "0x%04x: %s" $start [join $rendered " "]]
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
    set headerVersion [story_header_version $story]

    # A Z-machine story's first byte must be a defined story version. This keeps
    # unrelated .dat resources in mixed IF collections from being reported as VM
    # compatibility failures. Version 6 is a genuine Z story but intentionally
    # outside this text-only runtime's supported set.
    if {$headerVersion < 0} {
        incr failed
        set detail "unable to read Z-machine header version byte"
        puts [format "FAIL  %-32s %s" $name $detail]
        lappend failures [list $name $detail]
        continue
    }
    if {$headerVersion < 1 || $headerVersion > 8} {
        incr skipped
        puts [format "SKIP  %-32s not a Z-machine story (header version byte %d)" \
                  $name $headerVersion]
        continue
    }
    if {$headerVersion == 6} {
        incr skipped
        puts [format "SKIP  %-32s Z-machine Version 6 is intentionally unsupported by this text-only runtime" \
                  $name]
        continue
    }

    # Creating the session performs the remaining Z-machine header validation.
    if {[catch {zmachine::create $session $story} err]} {
        incr failed
        puts [format "FAIL  %-32s create: %s" $name $err]
        lappend failures [list $name "create: $err"]
        continue
    }

    set before [zmachine::info $session]
    if {[catch {zmachine::command $session $command} output options]} {
        incr failed
        set after [zmachine::info $session]

        # Some Tcl C APIs can return TCL_ERROR without setting a textual result.
        # Preserve Tcl's structured diagnostic fields in that case so the next
        # compatibility fix has something concrete to investigate.
        set detail $output
        if {$detail eq ""} {
            set pieces {}
            if {[dict exists $options -errorcode]} {
                lappend pieces "errorCode=[dict get $options -errorcode]"
            }
            if {[dict exists $options -errorinfo]} {
                set ei [string trim [dict get $options -errorinfo]]
                if {$ei ne ""} {
                    lappend pieces "errorInfo=$ei"
                }
            }
            set detail [join $pieces {; }]
            if {$detail eq ""} {
                set detail "TCL_ERROR with no result or diagnostic"
            }
        }

        set pcHex ""
        if {[dict exists $after pc]} {
            set pcHex [story_pc_hex_window $story [dict get $after pc]]
        }

        puts [format "FAIL  %-32s %s" $name $detail]
        lappend failures [list $name $detail $after $pcHex]
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
        if {[llength $failure] > 3 && [lindex $failure 3] ne ""} {
            puts "Bytes: [lindex $failure 3]"
        }
    }
}

# A nonzero exit makes this useful in ad-hoc compatibility scripts while still
# treating intentionally unsupported/non-Z catalog entries as skips.
exit [expr {$failed == 0 ? 0 : 1}]