# Evenk

A C++14 library for concurrent programming.

The primary target platform for now is Linux x86-64. Additionally it might
be used on Mac OS/X x86-64 but for the lack of the futex system call some
features do not function there.

The library itself is header-only so using it is possible just by copying
the header files wherever you like to include them from.

[ Currently the above is not true for "task.h" and "thread.h", these files
  depend on "config.h" that is generated with the steps described below. ]

The build step is needed for tests. It is required to have on your system
the following tools installed: automake, autoconf and, of course, make and
gcc or clang. As soon as you have these just run the following commands:

```
> ./bootstrap
> ./configure
> make
```