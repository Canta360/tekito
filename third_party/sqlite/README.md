# SQLite

This directory contains the SQLite 3.53.4 amalgamation obtained from the
official SQLite distribution. SQLite is public domain.

The TEKITO build uses `sqlite3.c` as a private static dependency of the
UserData layer. Core and TSF code do not include SQLite headers.
