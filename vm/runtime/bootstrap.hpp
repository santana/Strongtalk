/* Copyright 1994, 1995 LongView Technologies L.L.C. $Revision: 1.13 $ */
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

#ifndef _BOOTSTRAP_HPP
#define _BOOTSTRAP_HPP

#include "memory/allocation.hpp"

#include <cstdio>

// bootstrap parses and allocates memOops from a native 64-bit image file.
// The only supported format is the native format written by imageWriter
// (version 200 + Bytecodes::version()); sizes and method codes are already in
// the native 64-bit layout, so no 32-bit fixups or code relocation are needed.

class bootstrap : CHeapObj {
private:
  // Tables
  oop* oop_table;
  int number_of_oops;
  int max_number_of_oops;

  void initialize_tables(int initial_table_size);

  void add(oop obj);
  oop at(int index);

private:
  // File
  char* file_name;
  FILE* stream;
  //  int   file_size;
  char get_char();
  int get_integer();

  // Error handline
  bool _has_error;

  // Parsing
  void open_file();
  void parse_file();
  oop get_object();
  void close_file();
  void insert_symbol(memOop obj);

public:
  bootstrap(char* name);
  ~bootstrap();

  bool has_error() { return _has_error; }

  void read_oop(oop* oop_addr) { *oop_addr = get_object(); }

  char read_byte() { return get_char(); }
  doubleByte read_doubleByte() { return (doubleByte)get_integer(); }
  long read_long() { return get_integer(); }
  double read_double();

  long file_pos() { return ftell(stream); }

  // Plain unsigned base-128 integer (instruction counts, lengths, ...).
  int read_integer() { return get_integer(); }
  // 'I' + signed integer: a raw inline word (branch distance, DLL entry).
  int read_native_int();
};
#endif // _BOOTSTRAP_HPP
