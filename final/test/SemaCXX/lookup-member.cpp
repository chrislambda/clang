// RUN: %clang_cc1 -fsyntax-only -verify %s

namespace A {
  class String;
};

using A::String;
class String;

// rdar://8603569
union value {
char *String;
};
