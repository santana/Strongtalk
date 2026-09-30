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

#include "runtime/version.hpp"
#include "topIncludes/std_includes.hpp"
#include "utilities/ostream.hpp"

#include <stdio.h>
#include <string.h>

// All -D defines come from the Makefile; the fallbacks keep a hand-compiled
// or otherwise unusual build working instead of failing on undefined symbols.
#ifndef STRONGTALK_VERSION
#define STRONGTALK_VERSION "0.0.0-dev"
#endif
#ifndef STRONGTALK_GIT_HASH
#define STRONGTALK_GIT_HASH "unknown"
#endif
#ifndef STRONGTALK_GIT_DESCRIBE
#define STRONGTALK_GIT_DESCRIBE STRONGTALK_GIT_HASH
#endif
#ifndef STRONGTALK_BUILD_DATE
#define STRONGTALK_BUILD_DATE "unknown"
#endif
#ifndef STRONGTALK_BUILD_CONFIG
#define STRONGTALK_BUILD_CONFIG "unknown"
#endif

namespace {
const char* const kUnknown = "unknown";

bool is_unknown(const char* s) {
  return s == nullptr || s[0] == '\0' || strcmp(s, kUnknown) == 0;
}
} // namespace

const char* Version::version_string() {
  // Assembled from whichever parts the build system could supply, so a build
  // without git metadata (a source tarball, or a container image with no git
  // binary) still reports its build date and configuration. The buffers are
  // plain statics so this is safe to call before the VM is initialized.
  static char detail[192];
  static char buffer[256];
  const char* parts[3] = {git_describe(), build_date(), build_config()};

  size_t pos = 0;
  detail[0] = '\0';
  for (int i = 0; i < 3; i++) {
    if (is_unknown(parts[i]))
      continue;
    pos += snprintf(detail + pos, sizeof(detail) - pos, "%s%s", pos ? ", " : "", parts[i]);
    if (pos >= sizeof(detail)) // snprintf reports the length it *would* need
      break;
  }
  snprintf(buffer, sizeof(buffer), "%s (%s)", semantic_version(), detail);
  return buffer;
}

void Version::print_version(outputStream* s) {
  s->print("Strongtalk %s\n", version_string());
}