/*
Open 3D Stream

Copyright 2026 Open3DStream Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef OPEN3DSTREAM_O3DS_EXPORT_H
#define OPEN3DSTREAM_O3DS_EXPORT_H

//! O3DS_API marks the classes, functions and data that code outside the
//! core may use across a shared-library boundary.
//!
//! It is empty unless the build defines it. The CMake static library
//! (open3dstreamstatic) and every test and app built from it leave it
//! empty. The Unreal plugin compiles the core as its own module and defines
//! O3DS_API as that module's export macro (OPEN3DSTREAMCORE_API), so a
//! modular (DLL) editor build exports these symbols from the core module
//! and imports them everywhere else (docs/adr/0003).
//!
//! Annotate a class that has member functions defined in a .cpp file
//! (class O3DS_API Foo), and a free function or extern variable declared in
//! a header and defined in a .cpp file. Header-only classes and inline
//! functions need no annotation. An annotated class that owns a container
//! of move-only values (for example a std::map of std::unique_ptr) must
//! delete its copy operations explicitly: MSVC instantiates every member of
//! an exported class, including an implicit copy constructor that cannot
//! compile.
#ifndef O3DS_API
#define O3DS_API
#endif

#endif
