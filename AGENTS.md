# Repository Guidelines

## Project Structure & Module Organization

This repository is a C++ teaching database system. Main source code lives under `src/`, organized by subsystem: `record/` for record storage, `index/` for B+ tree indexes, `system/` for database and metadata management, `execution/` for SQL executors, `optimizer/` for planning, `analyze/` and `parser/` for SQL analysis/parsing, and `transaction/` / `recovery/` for transaction and log support. The command-line client is in `rmdb_client/`. Third-party dependencies are under `deps/`. Build outputs are generated in `build/`. Project documents and PDFs are kept at the repository root.

## Build, Test, and Development Commands

- `make -C build -j`: build all configured targets from the existing CMake build directory.
- `./build/bin/unit_test`: run the GoogleTest-based unit tests.
- `./build/bin/test_parser`: run parser smoke tests.
- `./build/bin/rmdb <db_name>`: start the database server for a local database.
- `./rmdb_client/build/rmdb_client`: start the interactive client if the client build exists.

If `cmake` is available and the build directory must be regenerated, use `cmake -S . -B build` before building.

## Coding Style & Naming Conventions

Use C++17 and follow the existing style. Classes use `PascalCase` such as `SmManager` and `IndexScanExecutor`; functions and variables generally use `snake_case`; member fields often end with `_`. Keep changes localized to the owning subsystem. Prefer existing helper APIs for metadata, record access, index handles, and condition evaluation instead of duplicating logic.

## Testing Guidelines

Add focused regression coverage for storage, index, and executor changes. For index work, always compare `SeqScanExecutor` and `IndexScanExecutor` results before and after `create index`, including output order. Cover equality, range predicates, missing keys, duplicate-key failures, delete/update index maintenance, and enough randomized inserts to trigger B+ tree splits.

## Commit & Pull Request Guidelines

Recent history uses short conventional-style messages such as `fix: ...`, `perf: ...`, and `improve: ...`. Keep commits scoped and avoid mixing unrelated local changes. Pull requests should describe the bug or feature, list touched modules, include commands run, and mention any remaining risks or untested paths.

## Agent-Specific Instructions

Do not overwrite existing user changes. Before editing, inspect `git status`; stage only files relevant to the task. For this repository, prefer correctness-preserving database behavior over cosmetic refactors.
