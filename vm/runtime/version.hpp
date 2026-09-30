/* Copyright 1994 - 1996, LongView Technologies L.L.C. $Revision: 1.1 $ */
/* Copyright (c) 2006, Sun Microsystems, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
	  disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Sun Microsystems nor the names of its contributors may be used to endorse or promote products derived
	  from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE


*/

#ifndef _VERSION_HPP
#define _VERSION_HPP

#include "memory/allocation.hpp"
#include "topIncludes/std_includes.hpp"

// Version identification of the VM, stamped in by the build system (see the
// VERSION_DEFINES block in the Makefile). All the strings are static, so they
// are safe to use before the VM is initialized -- e.g. from parse_arguments().
class Version : AllStatic {
public:
  // Semantic version of the nearest tag ("1.2.3"); falls back to a VERSION
  // file and finally to "0.0.0-dev" when there is no git checkout.
  static const char* semantic_version() { return STRONGTALK_VERSION; }
  // Short commit hash the binary was built from, or "unknown".
  static const char* git_hash() { return STRONGTALK_GIT_HASH; }
  // Full `git describe` string, e.g. "v1.2.3-42-g9f3a7b1-dirty".
  static const char* git_describe() { return STRONGTALK_GIT_DESCRIBE; }
  // ISO-8601 UTC build timestamp, or "unknown" when none was supplied.
  static const char* build_date() { return STRONGTALK_BUILD_DATE; }
  // The (arch-os-compiler) build configuration triple.
  static const char* build_config() { return STRONGTALK_BUILD_CONFIG; }
  // One-line summary: "1.2.3 (v1.2.3-42-g9f3a7b1, 2026-09-30T11:00:56Z, arm64-macos-clang)".
  // Parts the build system could not supply are left out. The result points
  // at a static buffer, valid until the next call.
  static const char* version_string();
  // Same, with a leading "Strongtalk ", printed to the given stream.
  static void print_version(outputStream* s = mystd);
};

#endif // _VERSION_HPP