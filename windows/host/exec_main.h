// exec_main.h -- --exec: an Android program an app started as a child process (exec_main.cpp).
#pragma once

namespace winapk::host {

// Runs the program the arguments after "--exec" name; does not return when the program starts (it
// ends the process). Every program the Bionic shim spawns from answers --exec with this first thing
// in its main (src/bionic/process.cpp starts its own executable again).
int exec_program_main(int argc, wchar_t** argv);

}  // namespace winapk::host
