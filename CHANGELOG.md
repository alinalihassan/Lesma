## [0.4.10](https://github.com/alinalihassan/Lesma/compare/v0.4.9...v0.4.10) (2022-03-14)


### Bug Fixes

* Fixed line continuation, added test ([9f70bb2](https://github.com/alinalihassan/Lesma/commit/9f70bb2baab0bfa59cd85dca0f2521c4acb19173))

## [0.4.9](https://github.com/alinalihassan/Lesma/compare/v0.4.8...v0.4.9) (2022-03-14)


### Bug Fixes

* Added line continuation '\' and newline ignore in paranthesis ([73d0134](https://github.com/alinalihassan/Lesma/commit/73d013470aabe0922fc413afc6db8b1ec86c7352))

## [0.4.8](https://github.com/alinalihassan/Lesma/compare/v0.4.7...v0.4.8) (2022-03-12)


### Bug Fixes

* Small memory improvements ([e48b4ca](https://github.com/alinalihassan/Lesma/commit/e48b4ca7d7748dddff537f8af594e339cb30d25c))

## [0.4.7](https://github.com/alinalihassan/Lesma/compare/v0.4.6...v0.4.7) (2022-03-12)


### Bug Fixes

* Fixed import not being parsed correctly ([d515f7b](https://github.com/alinalihassan/Lesma/commit/d515f7bad0dade02eee92a4e57cc1258537e5bee))

## [0.4.6](https://github.com/alinalihassan/Lesma/compare/v0.4.5...v0.4.6) (2022-03-11)


### Bug Fixes

* Added support for parenthesis in expressions ([485f2cf](https://github.com/alinalihassan/Lesma/commit/485f2cffb266719e4dcaa43f90df2b7e9be835be))

## [0.4.5](https://github.com/alinalihassan/Lesma/compare/v0.4.4...v0.4.5) (2022-03-11)


### Bug Fixes

* Fixed import incorrect span, fixed errors in import not being catched ([a0088a9](https://github.com/alinalihassan/Lesma/commit/a0088a934dd25c22d06ea91d4e1927a33bf26b7c))
* Fixed memory leak due to unused modules ([4d2282f](https://github.com/alinalihassan/Lesma/commit/4d2282fb71dfa2ff668b3759b28e737339508d13))
* Fixed memory leak from CLIOptions ([1693df5](https://github.com/alinalihassan/Lesma/commit/1693df5caa652683de5e0c4eb07f18f45335cff9))
* Fixed Target Machine leak ([b246e06](https://github.com/alinalihassan/Lesma/commit/b246e06a3dbf1cffb5ea1991018f52bc223b5519))
* Function call based on parameters ([b304917](https://github.com/alinalihassan/Lesma/commit/b3049173e0799e035eb351ecef0de3921d1977a2))

## [0.4.4](https://github.com/alinalihassan/Lesma/compare/v0.4.3...v0.4.4) (2022-03-10)


### Bug Fixes

* Fixed function parameters not being usable ([086a231](https://github.com/alinalihassan/Lesma/commit/086a231edcf57d4850bec8ff930918749b84bc26))

## [0.4.3](https://github.com/alinalihassan/Lesma/compare/v0.4.2...v0.4.3) (2022-03-10)


### Bug Fixes

* Fix cli options being true by default ([1cfbfa9](https://github.com/alinalihassan/Lesma/commit/1cfbfa9afff6b730b55b1bb5067d1f7fa13fb52b))

## [0.4.2](https://github.com/alinalihassan/Lesma/compare/v0.4.1...v0.4.2) (2022-03-10)


### Bug Fixes

* Added timer options alongside debug, fixed incorrect AST print ([19e45e7](https://github.com/alinalihassan/Lesma/commit/19e45e78a7e68eb4c3f9bd56b0dbe914b8e27975))

## [0.4.1](https://github.com/alinalihassan/Lesma/compare/v0.4.0...v0.4.1) (2022-03-10)


### Bug Fixes

* Improved Lexer and Parser error messages ([09a97f3](https://github.com/alinalihassan/Lesma/commit/09a97f3a673299f1808c4f5d7cf39dead25f0a61))

# [0.3.0](https://github.com/alinalihassan/Lesma/compare/v0.2.0...v0.3.0) (2022-03-08)


### Bug Fixes

* Import path is now relative to the calling file. Added test ([0af010d](https://github.com/alinalihassan/Lesma/commit/0af010da72a06dd6192bde2a244491a464d945df))


### Features

* Finished compilation step, fixed jit being executed regardless ([c451467](https://github.com/alinalihassan/Lesma/commit/c4514674c486baf4b016a9bff181e86f541a1f6e))
* Finished compilation step, fixed jit being executed regardless ([ba47c6c](https://github.com/alinalihassan/Lesma/commit/ba47c6c9fcf5e56842632d9c2c7a7ada5a2f07b6))
* Functional modules system for both AOT and JIT ([a8e9fee](https://github.com/alinalihassan/Lesma/commit/a8e9fee1f783e66f0152ddc18d9555899fb2e556))

# [0.2.0](https://github.com/alinalihassan/Lesma/compare/v0.1.0...v0.2.0) (2022-03-07)


### Bug Fixes

* Better handle cli positional arguments ([eded5c9](https://github.com/alinalihassan/Lesma/commit/eded5c9304bcda50cc56a419691001d933b2206d))


### Features

* Finished Linking method ([781536f](https://github.com/alinalihassan/Lesma/commit/781536f03208d2abe949a0c307d4e17b9dd10b69))
* Initial commit for Object Linking using Clang Driver ([efd2995](https://github.com/alinalihassan/Lesma/commit/efd29959fc841fb89669ef0da50063182bed8a98))

# [0.1.0](https://github.com/alinalihassan/Lesma/compare/v0.0.2...v0.1.0) (2022-03-03)


### Bug Fixes

* ✨ allow empty return in void functions ([b3bde14](https://github.com/alinalihassan/Lesma/commit/b3bde14d2b97469b6939753ee7a0c5db94ae963f))
* 🐛 Fixed tests not working after refactoring AST ([0495efd](https://github.com/alinalihassan/Lesma/commit/0495efd0968b4149227783be7c90959d864c395e))


### Features

* ✨ Added assign operators (+=, -=, *=, /=, %=) ([3fdfb63](https://github.com/alinalihassan/Lesma/commit/3fdfb6362d519c1b45b72d1f12be24585e3c6d55))

## [0.0.2](https://github.com/alinalihassan/Lesma/compare/v0.0.1...v0.0.2) (2022-03-02)


### Bug Fixes

* :bug: Debug statement acidentally pushed to main, reverted ([809c238](https://github.com/alinalihassan/Lesma/commit/809c23875cce744984bd06589e0b85bbcdcb1f37))
* :sparkles: else if keyword is now a proper multiword, not the elseif substitute ([19841df](https://github.com/alinalihassan/Lesma/commit/19841df85f2662c4c517528094f0fff5a8e2e19c))

## [0.0.1](https://github.com/alinalihassan/Lesma/compare/v0.0.0...v0.0.1) (2022-03-02)


### Bug Fixes

* Removed version from CMake, using github releases instead ([a6011b1](https://github.com/alinalihassan/Lesma/commit/a6011b11ef6e6b2da3150d0736901e3cb00b83f9))
