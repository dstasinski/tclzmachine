if {$argc != 1} {
    error "usage: smoke.tcl /path/to/tclzmachine.so"
}

load [lindex $argv 0] Tclzmachine
package require tclzmachine

if {[package present tclzmachine] ne "0.2.0"} {
    error "unexpected package version"
}

puts "tclzmachine package loaded successfully"
