## [0.8.2](https://github.com/alinalihassan/Lesma/compare/v0.8.1...v0.8.2) (2022-05-22)


### Bug Fixes

* Fixed defer not working at top level ([a5da7b2](https://github.com/alinalihassan/Lesma/commit/a5da7b2983501f5ea718038e397227e44363e73f))
* Fixed strings and other pointers not working properly ([06e975b](https://github.com/alinalihassan/Lesma/commit/06e975b729d301a787d2715eb837d58c0d2d8fe2))

## [0.8.1](https://github.com/alinalihassan/Lesma/compare/v0.8.0...v0.8.1) (2022-05-21)


### Bug Fixes

* Fixed comparisons between enums and other values not raising proper error ([c5fe7b6](https://github.com/alinalihassan/Lesma/commit/c5fe7b6a110883c35cae9d5fc5578018c75a8041))

# [0.8.0](https://github.com/alinalihassan/Lesma/compare/v0.7.0...v0.8.0) (2022-05-21)


### Bug Fixes

* Fixed different enums operators ([3e9dea7](https://github.com/alinalihassan/Lesma/commit/3e9dea749ed2e7baaf183476ef41ed258e9df7e3))


### Features

* Added class parsing for fields and methods ([cef3cad](https://github.com/alinalihassan/Lesma/commit/cef3cad94a4789b6cec81748c722d60fcdaad86e))
* Added dot access for enums, added fields to SymbolType for classes and enums ([9976dcf](https://github.com/alinalihassan/Lesma/commit/9976dcfd55c92ecac41e0436ed901a3a0172a123))
* Added enum equality comparation ([212fbcb](https://github.com/alinalihassan/Lesma/commit/212fbcbe8e519383ce9f0bd39f89bcab94137af5))
* Added initial classes with fields and methods. Added dot operator for fields and methods too ([57f0dff](https://github.com/alinalihassan/Lesma/commit/57f0dffe920be849e9aa370a2e6233509fdf42a9))

# [0.7.0](https://github.com/alinalihassan/Lesma/compare/v0.6.1...v0.7.0) (2022-05-18)


### Bug Fixes

* returns in if branches should no longer result in errors ([b80c074](https://github.com/alinalihassan/Lesma/commit/b80c07479fd4086efd4d2ea26be254922131d8bc))


### Features

* Added `is` and `is not` operators ([6841c0e](https://github.com/alinalihassan/Lesma/commit/6841c0ef8cb8c560ace0709509fcfbb8fc51cd42))
* Added defer keyword ([c1e8e92](https://github.com/alinalihassan/Lesma/commit/c1e8e926c8c345eb56d0769e671fe7cd8875e65d))
* Added function type parsing ([ed7f8c8](https://github.com/alinalihassan/Lesma/commit/ed7f8c8e76177e5be47ee989be2aafdf9bde26a9))

## [0.6.1](https://github.com/alinalihassan/Lesma/compare/v0.6.0...v0.6.1) (2022-05-17)


### Bug Fixes

* Fixed linker for compilation not working in macOS ([87a2751](https://github.com/alinalihassan/Lesma/commit/87a2751fd9c7706c0689a4413e072c5ec9ec667c))
* Fixed MacOS linking object files for AOT compilation ([bb20177](https://github.com/alinalihassan/Lesma/commit/bb201779c55d1388d6ede44a3b4f9e5a0877ced3))

# [0.6.0](https://github.com/alinalihassan/Lesma/compare/v0.5.0...v0.6.0) (2022-05-17)


### Bug Fixes

* Fix import path for std libs not being reflected in error ([3a82f97](https://github.com/alinalihassan/Lesma/commit/3a82f974a4ae427b3ea7d1d45d17aed291495c6c))
* Fixed extern functions clashing names, and trying to use wrong decl ([238406d](https://github.com/alinalihassan/Lesma/commit/238406db81d7bd902ede7ecff1ee888b7e108d6e))
* Fixes to lexer ([052c761](https://github.com/alinalihassan/Lesma/commit/052c761b092f3db5287dba8295b173e42b933e67))
* Improved lexer performance ([ef4b502](https://github.com/alinalihassan/Lesma/commit/ef4b502c35e20d5d768e0adfb6df57e1c52ddba4))
* Improved memory usage and Lexer & Parser performance ([043ab55](https://github.com/alinalihassan/Lesma/commit/043ab55580ab7674ce9e91a73857851099caeb7f))
* Improved types, added types to SymbolTable ([8cce05f](https://github.com/alinalihassan/Lesma/commit/8cce05f1408a06107e3162edb4464ae39f8aafab))
* Removed token state, refactored token class for lexer performance ([41547ed](https://github.com/alinalihassan/Lesma/commit/41547ed5cc758443934429666836296c19d256e3))
* Top level statements are now different, disallow them in blocks ([abb7d26](https://github.com/alinalihassan/Lesma/commit/abb7d26d424442ecc93e3aeacc5440d591c0beb3))
* Updated enums not being added as symbol table entries ([833ca38](https://github.com/alinalihassan/Lesma/commit/833ca38e19553bf4c80c6513a44df590d2fa7bc1))


### Features

* Added initial Enum configuration, dot access pattern and type scope not implemented yet ([aed2cd9](https://github.com/alinalihassan/Lesma/commit/aed2cd99b4a2dbe25cbb910c66b6e9f26d35e9ec))
* Added support for symbol types ([8981814](https://github.com/alinalihassan/Lesma/commit/8981814dcbb3cfd6f345bc94b713efc176d057a7))
* Custom types are now referenced properly ([1d8297f](https://github.com/alinalihassan/Lesma/commit/1d8297f4e1b574cc8b197e64e474f65adb9fd502))
* Improved file read times by 10x and lexer times by 10% ([f8e8382](https://github.com/alinalihassan/Lesma/commit/f8e83821269b0c00fe037d55adb455bfa2709cdb))
* Improved file reads by 10x using SourceManager class ([59ce186](https://github.com/alinalihassan/Lesma/commit/59ce18673aad8a0bfbfda140fa6f064643af2c31))

# [0.5.0](https://github.com/alinalihassan/Lesma/compare/v0.4.10...v0.5.0) (2022-05-06)


### Bug Fixes

* Fixed relative imports not working ([cea1f6e](https://github.com/alinalihassan/Lesma/commit/cea1f6ee592a3950ce300c22f9794a7982d77dcc))
* fixed tests ([0774a19](https://github.com/alinalihassan/Lesma/commit/0774a19168a650ea97d686ba511b28bc175a79ad))
* fixed tests ([8f45346](https://github.com/alinalihassan/Lesma/commit/8f453465b5866c48d0b610f6a2e6fe4526322f4c))
* Moved optimization phase after LLVM IR debugging print ([c785973](https://github.com/alinalihassan/Lesma/commit/c78597309da26e07d7d55565ec44bf2e929805b4))
* Removed print from keywords ([df1ba9a](https://github.com/alinalihassan/Lesma/commit/df1ba9a6d624582791de7c6220e783e61e2c9e0d))
* Removed print_str example, since current impl depends on output ([09e5f36](https://github.com/alinalihassan/Lesma/commit/09e5f36c8c0792649e960ab7dedd6c2d7f64d283))


### Features

* Added a few escape sequences ([1ddba5d](https://github.com/alinalihassan/Lesma/commit/1ddba5dfdf5ea88d1b68cc2e67c04c0e82c2b84d))
* Added basic string support ([caa3c25](https://github.com/alinalihassan/Lesma/commit/caa3c251c15a5fcd7758a56b5ffa82313185a49f))
* Added default import to base.les ([09c0d70](https://github.com/alinalihassan/Lesma/commit/09c0d708711501683326797e062335642fcd625b))
* Added std imports ([be3e0eb](https://github.com/alinalihassan/Lesma/commit/be3e0eb3ebc2e02547407344988247d08a17a38d))
* Symbols can now be overwritten in the scope ([bad0a28](https://github.com/alinalihassan/Lesma/commit/bad0a28be04d191afdf6b3ec8356e55ca1a7ce06))

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
