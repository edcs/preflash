# Changelog

## [0.4.0](https://github.com/edcs/preflash/compare/v0.3.0...v0.4.0) (2026-11-26)


### Features

* build and run the runtime on Linux ([#58](https://github.com/edcs/preflash/issues/58)) ([4dd731c](https://github.com/edcs/preflash/commit/4dd731c14d03aedb05892c19955382db22681fa3))
* cap a build's parallel compiles with PREFLASH_JOBS ([#66](https://github.com/edcs/preflash/issues/66)) ([0e98981](https://github.com/edcs/preflash/commit/0e98981819566224c21a116db54f7a785b6908be))
* keep Preflash's files in the Linux cache folder ([#63](https://github.com/edcs/preflash/issues/63)) ([ef56b50](https://github.com/edcs/preflash/commit/ef56b50863db91a05496128a67b3cee001522fde))
* keep the app's sockets safe from SIGPIPE on Linux ([#59](https://github.com/edcs/preflash/issues/59)) ([f885419](https://github.com/edcs/preflash/commit/f885419a3b3593ee4164eb156df92afe8ae13aa0))
* name crash frames, debug and copy on Linux ([#62](https://github.com/edcs/preflash/issues/62)) ([46e156b](https://github.com/edcs/preflash/commit/46e156ba7cb52f31b7514eeec767818b1406e5b3))
* write crash reports on Linux ([#61](https://github.com/edcs/preflash/issues/61)) ([ffd3d8a](https://github.com/edcs/preflash/commit/ffd3d8ade979b976ea424675f305be988a75aa5e))


### Bug Fixes

* keep the codec's state alive through exit ([#57](https://github.com/edcs/preflash/issues/57)) ([895606c](https://github.com/edcs/preflash/commit/895606cd97c26cef211677c009fb38d469e9a346))
* keep the speaker's lock alive while the app exits ([#69](https://github.com/edcs/preflash/issues/69)) ([15aee7c](https://github.com/edcs/preflash/commit/15aee7ca16c4b665011774d34252085426fbfaa2))
* stop Linux hanging on exit and unwinding deleted tasks ([#60](https://github.com/edcs/preflash/issues/60)) ([3f0d3eb](https://github.com/edcs/preflash/commit/3f0d3eb4215ff63eec0f56c1f07421df45a172a1))
* time a missing socket from when it went before calling a quit done ([#68](https://github.com/edcs/preflash/issues/68)) ([d191002](https://github.com/edcs/preflash/commit/d1910020bf47b75d38fc1457279139435f76e646))

## [0.3.0](https://github.com/edcs/preflash/compare/v0.2.0...v0.3.0) (2026-11-25)


### Features

* add the preflash web page ([#49](https://github.com/edcs/preflash/issues/49)) ([b6f210f](https://github.com/edcs/preflash/commit/b6f210f8d1f5cd819898a814a5d0ba0fea77262c))
* play the device's sound in the browser ([#55](https://github.com/edcs/preflash/issues/55)) ([d4855f0](https://github.com/edcs/preflash/commit/d4855f0203a6d29822a125243bea936cb5963eee))
* send the device's sound to the browser as Opus ([#54](https://github.com/edcs/preflash/issues/54)) ([ebb73e5](https://github.com/edcs/preflash/commit/ebb73e5bb77875cb19a5692a31a9e1764af09ccb))
* serve the device to a browser ([#47](https://github.com/edcs/preflash/issues/47)) ([7d920e2](https://github.com/edcs/preflash/commit/7d920e2c85be1eca066be1d13d473fe55ec6095b))
* serve the terminal app to a browser ([#48](https://github.com/edcs/preflash/issues/48)) ([beb70ca](https://github.com/edcs/preflash/commit/beb70cae7713f828723514a2d82e3cc7d4009d8f))
* show the panel at its own size, with zoom, a movable split and a toolbar ([#50](https://github.com/edcs/preflash/issues/50)) ([bd5b357](https://github.com/edcs/preflash/commit/bd5b357db125b26462f8438341e9971e71602911))
* stream the codec's sound on the control socket ([#53](https://github.com/edcs/preflash/issues/53)) ([3ab70ef](https://github.com/edcs/preflash/commit/3ab70ef161a0b271e025789e5fbca90398293422))


### Bug Fixes

* start a build folder's CMake cache afresh when the toolchain changes ([#46](https://github.com/edcs/preflash/issues/46)) ([1efe32a](https://github.com/edcs/preflash/commit/1efe32a4463f91c23d3552acac1915fdff3cfd61))

## [0.2.0](https://github.com/edcs/preflash/compare/v0.1.0...v0.2.0) (2026-11-25)


### Features

* add a toolbar, status bar and rotation to the device window ([#37](https://github.com/edcs/preflash/issues/37)) ([8539891](https://github.com/edcs/preflash/commit/85398911e129d0869583ded16da5c28ac6207b9e))
* add board buttons ([#38](https://github.com/edcs/preflash/issues/38)) ([8e1c189](https://github.com/edcs/preflash/commit/8e1c18984e7466b4b1bae4525ffe575747141955))
* add pointer input and a frames stream to the control socket ([#36](https://github.com/edcs/preflash/issues/36)) ([73c9084](https://github.com/edcs/preflash/commit/73c9084dd3e3f1802dddff8a39a7553cc0194021))
* model reset, power and download mode ([#39](https://github.com/edcs/preflash/issues/39)) ([9580260](https://github.com/edcs/preflash/commit/958026039b11817c671ce76b497ace82a93a09bc))
* run the device window in its own process ([#35](https://github.com/edcs/preflash/issues/35)) ([11b7c52](https://github.com/edcs/preflash/commit/11b7c52aa4bc9dc6fde3715a9a17368abd051984))


### Bug Fixes

* name a silent socket holder and forget hung-up port descriptors ([#44](https://github.com/edcs/preflash/issues/44)) ([b633ef5](https://github.com/edcs/preflash/commit/b633ef5286df321285d01a310fdacd78251f0151))
* send a subscribe reply only once its streams are live ([#45](https://github.com/edcs/preflash/issues/45)) ([92f096d](https://github.com/edcs/preflash/commit/92f096d10ffcc880eb5deb0be052544be599c033))

## 0.1.0 (2026-11-04)


### Features

* add a control socket and command line ([#18](https://github.com/edcs/preflash/issues/18)) ([a4b0573](https://github.com/edcs/preflash/commit/a4b0573d01a77fe02a20ae19614e6b7d37dad72f))
* add a demo app and benchmark ([#24](https://github.com/edcs/preflash/issues/24)) ([d799c3b](https://github.com/edcs/preflash/commit/d799c3b2fcd78a037bc238b8f1f3ccbb19d3645a))
* add a virtual serial port ([#20](https://github.com/edcs/preflash/issues/20)) ([5deb861](https://github.com/edcs/preflash/commit/5deb861a48d40f68e4809513f865d1dc09416b25))
* add audio and the io expander ([#9](https://github.com/edcs/preflash/issues/9)) ([7de23f1](https://github.com/edcs/preflash/commit/7de23f1a5e2a2b576b95b2c257bbaccea1bd846f))
* add debug views and forced failures ([#22](https://github.com/edcs/preflash/issues/22)) ([116a301](https://github.com/edcs/preflash/commit/116a301ffebf8a5be515dc7e4ba6ff905968c325))
* add flash partitions registered at run time ([#33](https://github.com/edcs/preflash/issues/33)) ([06a43c0](https://github.com/edcs/preflash/commit/06a43c013909d4f7bea2ccdf3f475edeab525d49))
* add freertos and system stand-ins ([#4](https://github.com/edcs/preflash/issues/4)) ([f7128fa](https://github.com/edcs/preflash/commit/f7128fae646ac1358f4062b500f31ecec9245170))
* add hardware checks ([#25](https://github.com/edcs/preflash/issues/25)) ([5561b85](https://github.com/edcs/preflash/commit/5561b85b109e8d7ba50d6d609a0229e15a375809))
* add nvs and console stand-ins ([#6](https://github.com/edcs/preflash/issues/6)) ([3399eed](https://github.com/edcs/preflash/commit/3399eed8179380bbb4db3149a69ff2be65a1c6c3))
* add sanitizer builds and crash reports ([#23](https://github.com/edcs/preflash/issues/23)) ([4bbfe02](https://github.com/edcs/preflash/commit/4bbfe023c6099fbd9522461862c056d0c8b53160))
* add the 3.49 V2 board and direct esp_lcd ([#21](https://github.com/edcs/preflash/issues/21)) ([1803a29](https://github.com/edcs/preflash/commit/1803a29c00c8d59e34c10fcddd38c46d6b7146dc))
* add the i2c bus and first chip models ([#8](https://github.com/edcs/preflash/issues/8)) ([131846a](https://github.com/edcs/preflash/commit/131846aca7d2e7a1410bfb3a9fbdd41e6f8fa84c))
* add the network and ota stand-ins ([#10](https://github.com/edcs/preflash/issues/10)) ([56f6135](https://github.com/edcs/preflash/commit/56f6135460e3d99cbfe73ef24106e8405273027f))
* add the terminal app ([#19](https://github.com/edcs/preflash/issues/19)) ([c5d3c38](https://github.com/edcs/preflash/commit/c5d3c38033316d46a558470aae773104e62a42c1))
* add the virtual panel and window ([#7](https://github.com/edcs/preflash/issues/7)) ([ae50adf](https://github.com/edcs/preflash/commit/ae50adf210192538b474d62c06d27b46f4d6da83))
* add timers, the task watchdog and task self-deletion ([#16](https://github.com/edcs/preflash/issues/16)) ([dce113c](https://github.com/edcs/preflash/commit/dce113cd8ab434be8de201c37f0c097709ac17cb))
* build unmodified esp-idf projects natively ([#2](https://github.com/edcs/preflash/issues/2)) ([a0f2a23](https://github.com/edcs/preflash/commit/a0f2a23f5cc27abde2957b86a5dd4bc2919a404f))
* load board files with extends and changes ([#5](https://github.com/edcs/preflash/issues/5)) ([ac695fd](https://github.com/edcs/preflash/commit/ac695fd95eddddc768700c4524d35c77621300cf))
* run snoozel end to end with preflash run ([#11](https://github.com/edcs/preflash/issues/11)) ([50a0440](https://github.com/edcs/preflash/commit/50a0440e805a2eb4ced11e731fe354ba1b33e7fd))
* ship releases with release-please and an install script ([#27](https://github.com/edcs/preflash/issues/27)) ([ae876e9](https://github.com/edcs/preflash/commit/ae876e9fc5a6b1beaa1d1068fce52a08f426c895))


### Bug Fixes

* bring os and console stand-ins closer to the device ([#13](https://github.com/edcs/preflash/issues/13)) ([84a5379](https://github.com/edcs/preflash/commit/84a53794340071f4272cef113c4d2f9cc44c41cb))
* close chip, audio and event loop gaps ([#14](https://github.com/edcs/preflash/issues/14)) ([b546f93](https://github.com/edcs/preflash/commit/b546f93545f14d85861975c698f68ad819303cfc))
* harden project builds and board parsing ([#12](https://github.com/edcs/preflash/issues/12)) ([f4c9e5c](https://github.com/edcs/preflash/commit/f4c9e5c036d376266c94a002c68f9d111d960fd7))
