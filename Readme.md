## Configuration:
The following Preprocessor Options can be used to control the build:
```
USE_INTERNAL_NULL_INDICATOR
USE_LLVM_SELECT
USE_NEW_EXTRACT
LOAD_NULLABLE
```

## Build:
Create a build directory and invoke cmake inside that directory:
```
cmake -DCMAKE_BUILD_TYPE=Release <source path>
make
```

## Run:
Remember to put a directory named `tables` with the database data inside the build directory.
Invoke the llvm-prototype executable without any additional arguments.

## SQL shell:
`sql` starts an empty in-memory database with an interactive prompt; `sql FILE...` executes the files and quits.
Statements end with `;` and may span several lines. The arrow keys scroll through earlier statements,
which are kept in `~/.tardisdb_history`. `\?` lists the backslash commands: `\q` (quit), `\i FILE`
(execute a file), `\d` and `\d TABLE` (tables and their columns), `\b` (branches) and `\timing`.

## Using Schemas:
The default SQL Schema, namely `schema.sql`, resides inside the 'schemas' directory.
This file will be treated like any other source file,
therefore a simple rebuild is enough to reflect any change to this file.
