# Type cases: the claims in docs/09-type-design.md, written as code

Each file is a translation unit compiled with the tree's OWN flags, `-fsyntax-only`.
The suffix is the expectation:

  `.refuse.c`  the compiler MUST reject it -- the mistake the type exists to stop
  `.accept.c`  the compiler MUST accept it -- the legal form beside it

BOTH HALVES, ALWAYS. A `.refuse.c` on its own cannot distinguish "the type refused
this" from "the header stopped compiling", so every claim carries an `.accept.c`
that fails the gate if the surface merely broke.

A case may declare the flag its refusal depends on:

  // REQUIRES: -Werror=implicit-int-conversion

When the compiler in use does not have that flag (gcc has no int-to-enum narrowing
diagnostic), the case NARROWS and is reported as unchecked rather than failing.
