## [0.18.1](https://github.com/alinalihassan/Lesma/compare/v0.18.0...v0.18.1) (2023-05-01)


### Bug Fixes

* Fixed parser not parsing power expression (x^y) ([ea0bc42](https://github.com/alinalihassan/Lesma/commit/ea0bc42e163ccd4b52c6c6840b5392ff7d85f879))
* fixed standard library integer print. Function lookup doesn't take type width into account yet ([2652777](https://github.com/alinalihassan/Lesma/commit/2652777825e0630ff27ce4a25f37f0c75f0b2ac4))

# [0.18.0](https://github.com/alinalihassan/Lesma/compare/v0.17.7...v0.18.0) (2023-04-30)


### Bug Fixes

* added custom operator for type equality. Fixed recursive type comp ([1bcccf7](https://github.com/alinalihassan/Lesma/commit/1bcccf7f7eec70eaf9cce6919d39bdaecea0618e))
* back to shell script for tests. Compilation unstable w/ Catch2 ([33ade8c](https://github.com/alinalihassan/Lesma/commit/33ade8ceca09183d77a5cf085aa60afc075f718d))
* fixed compiled object files declaring unmangled names ([14b819c](https://github.com/alinalihassan/Lesma/commit/14b819c62ef0a51e1077b2ac43b8dc15c1dd29da))
* fixed crash, benchmarks now run properly ([ddc0bdf](https://github.com/alinalihassan/Lesma/commit/ddc0bdf801faa3df7e5abddd91595c1b64e6974d))
* fixed external functions not importing properly ([e12658a](https://github.com/alinalihassan/Lesma/commit/e12658a84a3968ab8bd248ba8007dc73c41b3e47))
* fixed importing methods ([6cf7681](https://github.com/alinalihassan/Lesma/commit/6cf768115d04929e36003b78f9141c607eafae7e))
* fixed type comparison having errors ([8d85145](https://github.com/alinalihassan/Lesma/commit/8d85145c71bffc554cdf1bb9ad6a16796acbfd57))
* linking against c standard library when compiling now ([5de762a](https://github.com/alinalihassan/Lesma/commit/5de762a14daa38f6aa14e85a366d09afe715c3dc))
* moved from ordered maps to unordered maps and multimaps ([41d1d76](https://github.com/alinalihassan/Lesma/commit/41d1d76a9e7c046b7fef80da60e80a0cb529f927))
* separated JIT preparation and the actual execution of main, LLVM 16 ([e9adcd5](https://github.com/alinalihassan/Lesma/commit/e9adcd5f0d044b46981135aaa4545936b2d9bfe8))


### Features

* default values for functions now fully working! ([118f179](https://github.com/alinalihassan/Lesma/commit/118f179dda812a447cb8dc406768f28a1f08f92d))
* initial migration to google test ([fde19d3](https://github.com/alinalihassan/Lesma/commit/fde19d30d06b9116968497db719d1fd09d2f7e00))
* migrated sample JIT to LLJIT. ([e60f5ed](https://github.com/alinalihassan/Lesma/commit/e60f5eda2496f3940d5575a62b11b57723f519a1))


### Performance Improvements

* improved Lexer performance in Driver, we're now streaming file ([8db6dac](https://github.com/alinalihassan/Lesma/commit/8db6dac18dec07db8078ba77eb488b8e79dcec93))

## [0.17.7](https://github.com/alinalihassan/Lesma/compare/v0.17.6...v0.17.7) (2023-04-22)


### Bug Fixes

* fixed CHANGELOG no longer being updated ([ee4d9b5](https://github.com/alinalihassan/Lesma/commit/ee4d9b58392b3bb5446c91b2b1b3e525abd14095))

## [0.17.5](https://github.com/alinalihassan/Lesma/compare/v0.17.4...v0.17.5) (2023-04-22)


### Bug Fixes

* trigger ci with improved workflow ([0edea4b](https://github.com/alinalihassan/Lesma/commit/0edea4b6923c90c234aea53a7ba45012883e6da7))

## [0.17.4](https://github.com/alinalihassan/Lesma/compare/v0.17.3...v0.17.4) (2023-04-22)


### Bug Fixes

* last ci fix ([fae4461](https://github.com/alinalihassan/Lesma/commit/fae44617d6f0b4e35f9f84b2c125111de1f8844c))

## [0.17.3](https://github.com/alinalihassan/Lesma/compare/v0.17.2...v0.17.3) (2023-04-22)


### Bug Fixes

* fix ci trying to make new release when it shouldn't ([30c0ddc](https://github.com/alinalihassan/Lesma/commit/30c0ddc74f52be781975d0a1eda07033740d39bc))
* trigger ci, updated docs ([2a3abf6](https://github.com/alinalihassan/Lesma/commit/2a3abf6c2185b44a54a27d2b9f19a9395976e594))

## [0.17.2](https://github.com/alinalihassan/Lesma/compare/v0.17.1...v0.17.2) (2023-04-22)


### Bug Fixes

* fixed ci vcpkg ([fbd1686](https://github.com/alinalihassan/Lesma/commit/fbd168687345ae1292753bf6e094e237e48903f3))

## [0.17.1](https://github.com/alinalihassan/Lesma/compare/v0.17.0...v0.17.1) (2023-04-22)


### Bug Fixes

* fix vcpkg version bump ([926dc2c](https://github.com/alinalihassan/Lesma/commit/926dc2c2117c36ed55c6fce24f468faf4691c170))

# [0.17.0](https://github.com/alinalihassan/Lesma/compare/v0.16.5...v0.17.0) (2023-04-22)


### Bug Fixes

* added optional build-llvm feature ([8229bf6](https://github.com/alinalihassan/Lesma/commit/8229bf623e36eef9fce3905ddbe5f0e731cfd0be))
* fixed dot operator not having proper error highlight ([322847f](https://github.com/alinalihassan/Lesma/commit/322847f4e9d0dfb1cd2f2a53c00946e31a7a9962))
* Fixed enums ([f6a454f](https://github.com/alinalihassan/Lesma/commit/f6a454fad55754e28a8a43994c649f2f4ca5491e))
* fixed imports for new lesma Values and Types refactor ([556bbcf](https://github.com/alinalihassan/Lesma/commit/556bbcfaae9fd99ba522b11b05bdba4a65f47140))
* fixed is operator not comparing properly ([c0c78d6](https://github.com/alinalihassan/Lesma/commit/c0c78d6ddf8704057c68250ac777390aadbd843f))
* fixing clang format on PRs ([9fbbf08](https://github.com/alinalihassan/Lesma/commit/9fbbf086839fdf25c265561fb505c49525299fae))
* testing vcpkg ci/cd pipeline and CMakePresets ([82f633f](https://github.com/alinalihassan/Lesma/commit/82f633f717af64f63a1d25116694bfea6e9fbc10))
* updated CMake version above LLVM one ([d519959](https://github.com/alinalihassan/Lesma/commit/d519959430d5b3243b03dc0165087d200c841120))


### Features

* Added support for failing cases ([40f88f9](https://github.com/alinalihassan/Lesma/commit/40f88f9f828695e7c2d5d9d2da17d2aae4847969))
* fixed classes implementation ([87d3699](https://github.com/alinalihassan/Lesma/commit/87d3699052e04f88e68e1fded584d366dd04be2b))
* massive refactoring, using our Value and Type classes instead of llvm's ([9fecfa6](https://github.com/alinalihassan/Lesma/commit/9fecfa65b010f22c67c9d982c3f8ea54778dfaf6))
* migrated to vcpkg, migrated from clang to lld for compilation ([18e2081](https://github.com/alinalihassan/Lesma/commit/18e2081d6e0dbeceddcd6ac7bc3981a6a8d2296c))
* remaking tests ([9a66738](https://github.com/alinalihassan/Lesma/commit/9a66738266fb3f8c9fe41dd38eae2c4ae7bc2a44))

## [0.16.5](https://github.com/alinalihassan/Lesma/compare/v0.16.4...v0.16.5) (2023-01-09)


### Bug Fixes

* adjusted build options to build only cli by default ([8015ffc](https://github.com/alinalihassan/Lesma/commit/8015ffc3ab34fabf3e48174ede2c40ea40c79836))

## [0.16.4](https://github.com/alinalihassan/Lesma/compare/v0.16.3...v0.16.4) (2022-09-16)


### Bug Fixes

* Fixed deallocation errors temp ([5bfb467](https://github.com/alinalihassan/Lesma/commit/5bfb467539c933c9fb15fbf8ab8f7dce048d2e1b))
* Fixed Driver memory leaks ([092b96b](https://github.com/alinalihassan/Lesma/commit/092b96b8e7ff7860721b6bf2a14b807a95540635))
* Fixed memory leaks from AST and Parser ([a029292](https://github.com/alinalihassan/Lesma/commit/a02929242f595a3683b5d2d4f11ea036dbd8adb0))
* Fixed most Lexer memory leaks ([62c7dfd](https://github.com/alinalihassan/Lesma/commit/62c7dfde1b572a31ff0efc65b81ff8bd16a1a0de))
* Fixed symbol table memory leaks ([d66017e](https://github.com/alinalihassan/Lesma/commit/d66017ecf800eb5168221a2a9b0ec93dd6c65cbc))
* Fixed while loop deleting body memory twice ([cd031fb](https://github.com/alinalihassan/Lesma/commit/cd031fbfb5f271c20802991c30ad4df655f444b3))

## [0.16.3](https://github.com/alinalihassan/Lesma/compare/v0.16.2...v0.16.3) (2022-09-03)


### Bug Fixes

* Test fix for semantic release ([12a77a4](https://github.com/alinalihassan/Lesma/commit/12a77a48d52c387b294433767cea74e1138415f2))

## [0.16.2](https://github.com/alinalihassan/Lesma/compare/v0.16.1...v0.16.2) (2022-08-30)


### Bug Fixes

* Fixed tests error ([9dda15e](https://github.com/alinalihassan/Lesma/commit/9dda15ea5b51400c5244a4b47d320c55abcddabb))
* Updated installer to add to path and provide feedback ([2be4440](https://github.com/alinalihassan/Lesma/commit/2be44400dc7403709a72754a7db9528744264fa4))

## [0.16.1](https://github.com/alinalihassan/Lesma/compare/v0.16.0...v0.16.1) (2022-08-30)


### Bug Fixes

* changed release format ([f7e0645](https://github.com/alinalihassan/Lesma/commit/f7e064536cee76b17a27bf613bed703bfc05ceaf))

# [0.16.0](https://github.com/alinalihassan/Lesma/compare/v0.15.4...v0.16.0) (2022-08-30)


### Bug Fixes

* Can now use either local llvm or system-wide llvm ([45b6b83](https://github.com/alinalihassan/Lesma/commit/45b6b839329b16bf17f4350984389d47e57b16e3))
* fixed cmake path for dependency ([0b9b0bd](https://github.com/alinalihassan/Lesma/commit/0b9b0bd9564687fc90066c9e8545e50f7974fd70))
* Fixed library not able to make static lib ([a13bf75](https://github.com/alinalihassan/Lesma/commit/a13bf75edd2d6bc452b4a5f7ad7a3808bf84ef23))
* Fixed local llvm and clang ([81ce3a9](https://github.com/alinalihassan/Lesma/commit/81ce3a95f41e06b19a6c9593cfce9f3d5e6224cc))
* Fixed not adding include directories ([94071d8](https://github.com/alinalihassan/Lesma/commit/94071d874993b0c1d49716b3a346b2b0a9aca59b))
* We now move stdlib to .lesma directory in $HOME ([ff20761](https://github.com/alinalihassan/Lesma/commit/ff20761e5f36ef680296ee17c271e2de43868995))


### Features

* Added lesma as a library, ([cb5522e](https://github.com/alinalihassan/Lesma/commit/cb5522e4d7a7402c602f9ace503da15c41db86cc))

## [0.15.4](https://github.com/alinalihassan/Lesma/compare/v0.15.3...v0.15.4) (2022-08-26)


### Bug Fixes

* Attempt to fix Git version on GitHub ([eb3d547](https://github.com/alinalihassan/Lesma/commit/eb3d54704de2e92e026e32a2e807dc754e604c0c))
* Fixed checkout ([493d8e3](https://github.com/alinalihassan/Lesma/commit/493d8e3a3a13b01bb8a2ceca2ca8e30fea3d3c72))
* Fixed CI ([251fb4a](https://github.com/alinalihassan/Lesma/commit/251fb4aefe3cb1bfadc0752600f223c9145ceb8c))
* Updated CMake to properly get version from git ([5069520](https://github.com/alinalihassan/Lesma/commit/506952014894d7c2f3190b83abca4e1747b55bca))

## [0.15.3](https://github.com/alinalihassan/Lesma/compare/v0.15.2...v0.15.3) (2022-08-26)


### Bug Fixes

* Attempt to fix release assets and debug cmake issue ([52fb330](https://github.com/alinalihassan/Lesma/commit/52fb330998e836f4612abdd8ac1d2d6cb4685e9a))

## [0.15.2](https://github.com/alinalihassan/Lesma/compare/v0.15.1...v0.15.2) (2022-08-26)


### Bug Fixes

* Artifacts not being uploaded as assets properly ([d64ca01](https://github.com/alinalihassan/Lesma/commit/d64ca016f9917e72966f6a0ff33f695301485f00))

## [0.15.1](https://github.com/alinalihassan/Lesma/compare/v0.15.0...v0.15.1) (2022-08-26)


### Bug Fixes

* testing fixed CI ([4df0269](https://github.com/alinalihassan/Lesma/commit/4df0269e99a9c459556112083cdb3b4ff4aeedd5))

# [0.15.0](https://github.com/alinalihassan/Lesma/compare/v0.14.0...v0.15.0) (2022-08-26)


### Bug Fixes

* Fixed class import not working properly, optimized and simplified imports ([0491912](https://github.com/alinalihassan/Lesma/commit/04919124706721e54d6f5c068b09587149b29caf))
* Fixed exit not being imported (hacky for now) ([36eb565](https://github.com/alinalihassan/Lesma/commit/36eb565b7deef8fea7b7f174ab6b2880b3ef9dff))
* Fixed optimization being done with O0 ([5c73bf1](https://github.com/alinalihassan/Lesma/commit/5c73bf1236b9b7b3ef736474dcf0fa85a986bbd0))
* Fixed type comparisons across modules, fixed other asserts, contexts reused across modules ([953f6bc](https://github.com/alinalihassan/Lesma/commit/953f6bc44beb5b852c52989e74ccff20b4c6cbc6))
* Improved CLI help experience, added version ([c03f426](https://github.com/alinalihassan/Lesma/commit/c03f4263ab571aff5e8df821b5d0f1a371741107))
* Reverted test files back to explicitly defining exit extern ([93f5f33](https://github.com/alinalihassan/Lesma/commit/93f5f33964adf7386bf8902fb253dd5d4c0d1e14))


### Features

* Added local LLVM & Clang installation with bash script ([e6e45fe](https://github.com/alinalihassan/Lesma/commit/e6e45fe526c2013a91725cb3535f46cf3a640515))
* Fixed assertions because of different contexts, now we share the same ([1ff4f36](https://github.com/alinalihassan/Lesma/commit/1ff4f36ec2fc4f42218356c826b4373c2eec0262))

# [0.14.0](https://github.com/alinalihassan/Lesma/compare/v0.13.3...v0.14.0) (2022-08-02)


### Bug Fixes

* Fixed error messages when compiling multiple files not showing proper file ([a639e7c](https://github.com/alinalihassan/Lesma/commit/a639e7ca2d792ea4b83dcf2a743e1445744947d1))
* Fixed running strings would not report errors properly ([f237c28](https://github.com/alinalihassan/Lesma/commit/f237c2807ef3e009d91a21691f02e8b91a194f12))
* Fixed tests and stdlib debug code ([4daefe0](https://github.com/alinalihassan/Lesma/commit/4daefe0d18b68732634eccf2b9fbb7982f8091e7))


### Features

* Added driver class to be used by other projects (server wink wink) ([6d1be42](https://github.com/alinalihassan/Lesma/commit/6d1be426e1ece3c0fd87d32f4161e1e402978fbc))
* Added export keyword, fixed classes and enums not being imported in compilation ([e83055b](https://github.com/alinalihassan/Lesma/commit/e83055b460b6f0e7f0d51ebf08fad0ae405e566e))
* Added python-like import syntax, LLVM 14 ([c9012ea](https://github.com/alinalihassan/Lesma/commit/c9012eae3d6adc8fcb61ed1e09bfc8b5e7f5d6e3))

## [0.13.3](https://github.com/alinalihassan/Lesma/compare/v0.13.2...v0.13.3) (2022-06-01)


### Bug Fixes

* Removed release binaries ([be95705](https://github.com/alinalihassan/Lesma/commit/be95705b00f725b6653defac79e5c4be9838029b))

## [0.13.2](https://github.com/alinalihassan/Lesma/compare/v0.13.1...v0.13.2) (2022-06-01)


### Bug Fixes

* Release ([816cece](https://github.com/alinalihassan/Lesma/commit/816cece50c0f83184211d25f675673543016f007))

## [0.13.1](https://github.com/alinalihassan/Lesma/compare/v0.13.0...v0.13.1) (2022-06-01)


### Bug Fixes

* Fixed assets not being added to release ([c68fead](https://github.com/alinalihassan/Lesma/commit/c68fead25b883a67935a8625796774826a2150e6))

# [0.13.0](https://github.com/alinalihassan/Lesma/compare/v0.12.0...v0.13.0) (2022-06-01)


### Bug Fixes

* Added function verification back ([f4ed1a8](https://github.com/alinalihassan/Lesma/commit/f4ed1a81698c1cd66749ccb33f13181147a37a04))
* Disable function verification again ([8242b47](https://github.com/alinalihassan/Lesma/commit/8242b4769585ee9cb32a2a1e43fe62411239ae2e))
* Fixed enum comparison not working as expected ([43d3ec8](https://github.com/alinalihassan/Lesma/commit/43d3ec898ab825133d11b03d38950296804895c3))
* Fixed not being able to compare pointer addresses ([60a6566](https://github.com/alinalihassan/Lesma/commit/60a65661622ffd7602e45f90ca6dbaa49520ffe8))
* Fixed not finding field in class having undefined behaviour ([f1b575f](https://github.com/alinalihassan/Lesma/commit/f1b575fb32cddc3f229a495b5bf431c1a73a6262))
* Fixed tests ([1d1d0e3](https://github.com/alinalihassan/Lesma/commit/1d1d0e36ab0bdd0d9abb2bbcbaefcd6bee195e71))
* Removed .tmp variable names in LLVM IR ([5af603a](https://github.com/alinalihassan/Lesma/commit/5af603a64b470b682c9f3ad2c50b697534c7ea80))


### Features

* Added check for functions that don't always return ([9510dbd](https://github.com/alinalihassan/Lesma/commit/9510dbd8c1639258d5efb6f0a724b02d1feffd44))
* Added check for return type to match function type ([7c284ff](https://github.com/alinalihassan/Lesma/commit/7c284ff53a15910801b4dc64d3154b5bfc6c2814))
* Added pretty print for AST ([e9d60c8](https://github.com/alinalihassan/Lesma/commit/e9d60c816682300816fa9c0e059291b678bfd070))

# [0.12.0](https://github.com/alinalihassan/Lesma/compare/v0.11.0...v0.12.0) (2022-05-28)


### Bug Fixes

* Fixed de-referencing primitive values ([5ebb733](https://github.com/alinalihassan/Lesma/commit/5ebb733c750d17c520a18f566c1cdc6c6de1e920))
* Temporary fix for code branches not having any more instructions ([1a83cf7](https://github.com/alinalihassan/Lesma/commit/1a83cf7ae1b8e340ec8b523b213ee4296d246266))


### Features

* Added bitcast for pointers of integer or void types ([95a45c4](https://github.com/alinalihassan/Lesma/commit/95a45c48f7bea1326a00edb369ee92b3544ce205))
* Added low level types for better FFI interop ([19ee297](https://github.com/alinalihassan/Lesma/commit/19ee2978946795d3568b3a0ab363176efa7cda2f))
* Added null pointer representation ([5a8fb9c](https://github.com/alinalihassan/Lesma/commit/5a8fb9cef909aa80a299f06a9148b19f9f32e455))
* Added pointer and reference unary operators ([ce4ea17](https://github.com/alinalihassan/Lesma/commit/ce4ea174e3987717804b32c717c184640ec65f22))
* added pointers and references for low level implementations ([dc0569f](https://github.com/alinalihassan/Lesma/commit/dc0569fb0ba41250d34ffd38008b8c392dc82e47))
* Extended stdlib with string conversions, cmd input, and random ([8e03a0d](https://github.com/alinalihassan/Lesma/commit/8e03a0d4198074d316d4b970a5afae0a7f8690a6))
* External FFI functions are no longer imported ([47d2b8c](https://github.com/alinalihassan/Lesma/commit/47d2b8ca05cfa21838f50377f5280c98d86c8981))

# [0.11.0](https://github.com/alinalihassan/Lesma/compare/v0.10.1...v0.11.0) (2022-05-26)


### Features

* Functions can now refer to each other without having to be declared in a certain order ([7230883](https://github.com/alinalihassan/Lesma/commit/7230883d94f37b8bff490131dc6e6c3645aced46))

## [0.10.1](https://github.com/alinalihassan/Lesma/compare/v0.10.0...v0.10.1) (2022-05-26)


### Bug Fixes

* Fixed recursive functions not being able to call themselves in their body ([51541f4](https://github.com/alinalihassan/Lesma/commit/51541f403b3ddddd22c81c6a2690c422e06576f7))
* Fixed unary operator parsing ignoring left side ([81702f1](https://github.com/alinalihassan/Lesma/commit/81702f13cd5d0d25508e00656dd0d455097feeff))

# [0.10.0](https://github.com/alinalihassan/Lesma/compare/v0.9.3...v0.10.0) (2022-05-25)


### Bug Fixes

* Fixed being able to have a dangling comma in parameter list ([2ee6700](https://github.com/alinalihassan/Lesma/commit/2ee670078c09fc1aa53567393e1c8a3930fa723a))
* Fixed vardecl bug ([d65b145](https://github.com/alinalihassan/Lesma/commit/d65b145daa986de0156d9a522b0192d4f3f269be))
* Fixed variable declaration was creating the values twice ([a0df418](https://github.com/alinalihassan/Lesma/commit/a0df418ab875a4db7348517a78301907d4326ed3))


### Features

* Added varargs, currently only for extern functions, added print for all types ([510f78a](https://github.com/alinalihassan/Lesma/commit/510f78a4f9ecd5c5dad615c424a9a02665a665ca))
* Classes and Enums can now be imported properly from other modules ([dd78333](https://github.com/alinalihassan/Lesma/commit/dd783333074d489172315e38d56ac2ed240da673))
* Nested imports working properly, already read modules are skipped. ([c862e9a](https://github.com/alinalihassan/Lesma/commit/c862e9ac6a3d35436f65da70e949bdd0ca2370f5))

## [0.9.3](https://github.com/alinalihassan/Lesma/compare/v0.9.2...v0.9.3) (2022-05-24)


### Bug Fixes

* Don't create new extern functions if they are in the module and table ([751ff79](https://github.com/alinalihassan/Lesma/commit/751ff79cfb74b67cf7c03bf9ca31e66946ba2364))

## [0.9.2](https://github.com/alinalihassan/Lesma/compare/v0.9.1...v0.9.2) (2022-05-24)


### Bug Fixes

* Fixed method parameters not being in right order ([8674488](https://github.com/alinalihassan/Lesma/commit/8674488f463dafa8cff18afa6a92cb2e7b8ad2ca))
* Fixed not finding functions when called inside methods ([43386b6](https://github.com/alinalihassan/Lesma/commit/43386b61b6b21aa189403efe890755ed229f3d79))
* Temporary fix for checks failing when functions in other modules not being imported properly ([972fd15](https://github.com/alinalihassan/Lesma/commit/972fd15d624cec1283bb365c521f10a56bfc8ed6))

## [0.9.1](https://github.com/alinalihassan/Lesma/compare/v0.9.0...v0.9.1) (2022-05-24)


### Bug Fixes

* Fixed not being able to use enums and classes as parameters ([9c1f6a7](https://github.com/alinalihassan/Lesma/commit/9c1f6a7a660ec072c0952c08336e0d8a245497d5))

# [0.9.0](https://github.com/alinalihassan/Lesma/compare/v0.8.2...v0.9.0) (2022-05-24)


### Bug Fixes

* Fixed allowing constructors to return a value ([f6dfbba](https://github.com/alinalihassan/Lesma/commit/f6dfbba46d6215213c5c291981a6c2368313f1d0))
* Fixed parameter pointers not being handled properly ([55e86bc](https://github.com/alinalihassan/Lesma/commit/55e86bcefaff4610150f4453f6430fb80ac05e2c))
* Temporarily disallow classes with no constructors until we implement built-in ones ([5096314](https://github.com/alinalihassan/Lesma/commit/50963146e0cb7482f3d29da75c399063f61db100))


### Features

* Added storing values into class fields, added class constructor ([3675e51](https://github.com/alinalihassan/Lesma/commit/3675e51ba51a89f892f042012df401ec9765d7ca))

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
