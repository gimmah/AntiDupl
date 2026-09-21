# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [2.3.15.0] - 2026-09-21
### Added
- HEIF/HEIC image format support merged from `dev`, including RGB/RGBA to BGRA
  conversion, a HEIF search type in the GUI and the libheif version display in
  the About window.
- New folder picker dialog, brought in from `dev`.

### Changed
- Merged `master` (through v2.3.14) into `dev`, bringing the branch up to date
  with AVIF/JPEG XL support, the vcpkg-based build, and the CI pipeline.
- Project upgraded to target .NET 10 / build with Visual Studio 2022+.

### Fixed
- Memory release functions to prevent memory leaks in HEIF handling.

## [2.3.14.0] - 2026-09-14
### Fixed
- Crash when clearing the search path list.
- Switch fallthrough bug in defect `PERFORM_HINT` handling.
- Broken AVIF decoding; dropped the unused GPL `x265` dependency.

### Changed
- CI build performance and vcpkg binary-cache reliability improvements.

## [2.3.13.0] - 2026-04-11
### Added
- Ignore specific files by regular expression.
- New hotkey option for opening the image diff tool.
- Highlight filename differences in red (vertical mode).

### Changed
- Portable mode now sets the user path to the application's startup path.
- Windows 10 fix: use `LocalApplicationData` for the non-portable version.
- Improved English documentation readability; links upgraded to HTTPS.

### Fixed
- `IndexOutOfRangeException` in `ResultsListView.MakeAction`.
- Hotkeys menu cut off in some configurations.
- Older-version hotkeys silently taking over new bindings.
- Missing file extensions in the file selection window.

## [2.3.12.0] - 2023-10-10
### Added
- AVIF and JPEG XL (JXL) image format support, including a search type in the
  GUI and version display in the About window.
- Reference to AntiDuplX.
- German language file.

### Changed
- Bumped `Microsoft.Windows.Compatibility` dependency.

## [2.3.11.0] - 2023-03-15
### Added
- New folder picker implementation.

[Unreleased]: https://github.com/gimmah/AntiDupl/compare/v2.3.15.0...HEAD
[2.3.15.0]: https://github.com/gimmah/AntiDupl/compare/v2.3.14...v2.3.15.0
[2.3.14.0]: https://github.com/ermig1979/AntiDupl/compare/v2.3.13...v2.3.14
[2.3.13.0]: https://github.com/ermig1979/AntiDupl/compare/v2.3.12...v2.3.13
[2.3.12.0]: https://github.com/ermig1979/AntiDupl/compare/v2.3.11...v2.3.12
[2.3.11.0]: https://github.com/ermig1979/AntiDupl/releases/tag/v2.3.11
