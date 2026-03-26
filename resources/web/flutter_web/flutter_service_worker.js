'use strict';
const MANIFEST = 'flutter-app-manifest';
const TEMP = 'flutter-temp-cache';
const CACHE_NAME = 'flutter-app-cache';

const RESOURCES = {"flutter_bootstrap.js": "7943c870f1ae7933118baa23d8f2e46b",
"version.json": "e2923700fa3c3896d912a256cc15d2d7",
"main.dart.js_7.part.js": "92118f3253ec52ade922b34ec8bf4aec",
"main.dart.js_12.part.js": "1a3152c5016b6cc154b3a2a0bf9eaa70",
"main.dart.js_6.part.js": "b89b64415111f8311a12fc67e2eb3bfe",
"index.html": "2753effad264bd7ecad1460aa8a6e57f",
"/": "2753effad264bd7ecad1460aa8a6e57f",
"main.dart.js_13.part.js": "bd2b27cd6aa770ad357b2c05e6cd08a2",
"main.dart.js_4.part.js": "f5d74c5621c82179a22f45a37bbf50ce",
"main.dart.js": "e85b4a5b081b6f2329fbbbb9c6bc38ce",
"flutter.js": "f31737fb005cd3a3c6bd9355efd33061",
"main.dart.js_10.part.js": "bc01c525ec42c9a4eda1aa8aa27ab659",
"main.dart.js_5.part.js": "87186fbb7d030ba603cdee527317e6b3",
"main.dart.js_15.part.js": "015a8c4255675eda4701684e48b71c62",
"version.changelog": "342987a68a2c2325abdbf8c777f5f1f9",
"favicon.png": "be8d1ab28c20907c9869c345d0482962",
"main.dart.js_9.part.js": "2503a5a15220203dacdb4ff05282711c",
"main.dart.js_8.part.js": "6e43b3bfb637025278c342683b21549a",
"icons/Icon-192.png": "ab1f25ced1559729e334de938eae91a5",
"icons/Icon-maskable-192.png": "e41e8489c0f6a822acf8dab362e112b7",
"icons/Icon-maskable-512.png": "4870fb6720f4fcad016cb582589d136d",
"icons/Icon-512.png": "343022ac1c56796cb7ff635faf0646ef",
"manifest.json": "901d86fb8842ec0d66225a542131d689",
"main.dart.js_14.part.js": "5c227e467fde8f51f400cd6c6450a0de",
"main.dart.js_1.part.js": "be09942ff4e052f8b9b42b62bfb5716a",
"main.dart.js_3.part.js": "90483794f1a2fbd6b7c38b40ed1b37b2",
"main.dart.js_16.part.js": "13b3836245e44820c018dbbf1f0321a0",
"assets/AssetManifest.json": "ca44381a802747c6d8e89b023bac3507",
"assets/NOTICES": "82b5765280a5fc06877a11eed8de0142",
"assets/FontManifest.json": "0dc3d44d47c5e2636cdca4babafb2396",
"assets/AssetManifest.bin.json": "dcec627f50d0213c20b6739bb90ed6af",
"assets/packages/lava_model_station/assets/404.png": "e8a45c994c2f6f551cf1e052f64dba1c",
"assets/packages/lava_model_station/assets/placehoder-image.png": "fd0b3547e2b90c124112401d7a8f6a02",
"assets/packages/lava_model_station/assets/svgs/iconPlate.svg": "05acab23d908534f0660b4f06cc36e8d",
"assets/packages/lava_model_station/assets/svgs/iconTime.svg": "bcddee2512587e441a39fbdf4b0c1d70",
"assets/packages/lava_model_station/assets/svgs/iconGcode.svg": "9b20a88a3f79c7347e665a8738eeea6b",
"assets/packages/lava_model_station/assets/svgs/iconNozzle.svg": "fa46a390b0c9db063e289af55b55329e",
"assets/packages/lava_model_station/assets/svgs/iconWeight.svg": "bfe3246df9e15f08bc5208d34ae3a814",
"assets/packages/lava_model_station/assets/empty-box.png": "f0fcb1ead826eec9ff565c2ac0dfd1da",
"assets/packages/lava_device_control/assets/files/filament.json": "f8f71f02e204c523b70e982d281f407d",
"assets/packages/cupertino_icons/assets/CupertinoIcons.ttf": "391ff5f9f24097f4f6e4406690a06243",
"assets/packages/fluttertoast/assets/toastify.js": "56e2c9cedd97f10e7e5f1cebd85d53e3",
"assets/packages/fluttertoast/assets/toastify.css": "a85675050054f179444bc5ad70ffc635",
"assets/packages/wakelock_plus/assets/no_sleep.js": "7748a45cd593f33280669b29c2c8919a",
"assets/shaders/ink_sparkle.frag": "ecc85a2e95f5e9f53123dcaf8cb9b6ce",
"assets/AssetManifest.bin": "5b4abe05212e5a1675361ee2aa3f362a",
"assets/fonts/MaterialIcons-Regular.otf": "165e0312a2248a5fe696bc445a5c4e9a",
"assets/assets/mock_data/model_detail.json": "9c3ce3e4bb3dbe78b74fc2d11d152485",
"assets/assets/mock_data/model_station_list.json": "1dcbd846d8b97bae543caba6ce135040",
"assets/assets/i10n/zh-CN.json": "b75b9cdfaa7f0dd8685f451947e720b9",
"assets/assets/i10n/en.json": "49a88f8ed551553ed2720b40174f76a7",
"assets/assets/images/deviceNoResponse.webp": "1ca23a7feedfdc34362ea5789ccf895b",
"assets/assets/images/deviceAuthorized.webp": "8eb814193bed15cec22658018871aba8",
"assets/assets/images/IpInputGuide.webp": "06c11ce1dadc2910676aec6d40a5eea5",
"assets/assets/images/deviceDisplayDefault.webp": "ea13b4cb58c0a38e8cf15f7033a07833",
"assets/assets/images/deviceAuthorizing.webp": "ad1b45a33b70fe2e551a343cab248de3",
"assets/assets/images/expectation.webp": "1fbebbcb5401021d91ae38864ec3a5c1",
"assets/assets/images/deviceNotConnected.webp": "3ccdf2ed043b26f62a4fa47e5ee69bdf",
"assets/assets/images/gcodeCover.png": "cd7e5c13429bc568b1c3ed8b3953ba86",
"assets/assets/images/deviceNoNetwork.webp": "e3f9e38c9bea5b8dd88f5729287bb23b",
"assets/assets/images/defaultGcode.webp": "f50f2b0c6bdbb42db1193c78a35c2958",
"assets/assets/images/printtaskDefault.png": "2d53add29c1647300def2ec024574307",
"assets/assets/images/deviceRejected.webp": "cb02f340100006ad22965c215fc6726c",
"assets/assets/images/ipInputGuideCN.webp": "6ff23fea6ac4212d222500ba1cbbddcf",
"assets/assets/images/deviceExceptionMessage.webp": "219c10258551fb8196a500d010fe6858",
"assets/assets/images/3dprinter.png": "4515f02e633d58a1119c2f55114da32b",
"assets/assets/images/logo.png": "b1a7a2105873616de883cf2662a2bf9c",
"assets/assets/images/controlDefault.png": "1122ed99c68469d307335e55350c2bff",
"assets/assets/images/filamentDefault.png": "e3b141369c0b9f85c880f4c070885222",
"assets/assets/images/defaultEmpty.png": "a0cd09ca961ed194dae6b02cde0230b2",
"assets/assets/images/deviceInvalidVersion.webp": "66e3b61ac908b900761bf014e92c1d3d",
"assets/assets/svgs/iconMainCooling.svg": "55b38461348e477abac33fdda8f98e32",
"assets/assets/svgs/iconCloseWhite.svg": "21e00e7b7a7031241d82983eef24b416",
"assets/assets/svgs/modelStation.svg": "dfd8ccb848b39df409d943c048c36a84",
"assets/assets/svgs/iconClose.svg": "f6db4c0e4369cc05ae28d3bea8d5b1ad",
"assets/assets/svgs/loginPlatformApple.svg": "be43d78435feca50bbabad292a1039c7",
"assets/assets/svgs/iconNotific.svg": "27082276596d830c36e1f5d0902b3929",
"assets/assets/svgs/iconHome.svg": "311c1b3d4498b05e641e171b1ff4f375",
"assets/assets/svgs/iconArrowRight.svg": "04a057ec0f5dbf3fa60be2625cc94379",
"assets/assets/svgs/loginPlatformGithub.svg": "a3e7a4e22db16c9c9305b3c709c2d14c",
"assets/assets/svgs/textfieldCode.svg": "b0b13793929d8ea2daefea50788b0580",
"assets/assets/svgs/textfieldPassword.svg": "2e3a0e88318b56043a1b258032ee540c",
"assets/assets/svgs/tabbar/iconMine.svg": "8c31cd203c81a95266cc30fe09937ed2",
"assets/assets/svgs/tabbar/iconServiceActive.svg": "0e09682f0c235f4d7b909fd3cf162891",
"assets/assets/svgs/tabbar/iconDiscoverDark.svg": "3caaceb51949746c56ae5d5bc205c585",
"assets/assets/svgs/tabbar/iconDeviceActive.svg": "76b799582124c5c5df23f3bfe83881bf",
"assets/assets/svgs/tabbar/iconDeviceDark.svg": "1b3f24760bb5803737314cbc2cc6b6d1",
"assets/assets/svgs/tabbar/iconMineDark.svg": "0a440809fbab2570292432f4e9c950b1",
"assets/assets/svgs/tabbar/iconService.svg": "2f49fff4b4ff0136667a32d6474ac6b9",
"assets/assets/svgs/tabbar/iconMineActiveDark.svg": "030b81bd2311b735940cfca9554ae04b",
"assets/assets/svgs/tabbar/iconServiceActiveDark.svg": "5148985bca2986c5e07ba7940c10afd1",
"assets/assets/svgs/tabbar/iconDiscoverActive.svg": "53ee419f392142d30b279754dc58ecf1",
"assets/assets/svgs/tabbar/iconServiceDark.svg": "d1273a072e99e19978ce169c51a06326",
"assets/assets/svgs/tabbar/iconMineActive.svg": "d5e1793a5a31a4884d1ad0534b621735",
"assets/assets/svgs/tabbar/iconDeviceActiveDark.svg": "0733885cdc97289ca12341320862e9de",
"assets/assets/svgs/tabbar/iconDevice.svg": "3a3acb433dd1e77848606e8ecf16c50d",
"assets/assets/svgs/tabbar/iconDiscover.svg": "291451c7c649c5966a00716a7233e1a5",
"assets/assets/svgs/tabbar/iconDiscoverActiveDark.svg": "f36a009bc608d7ef47cfcc3b3aac0559",
"assets/assets/svgs/videoImgPlay.svg": "7c96b94af00f79bf00e5b93552eed30d",
"assets/assets/svgs/loginPlatformFacebook.svg": "0a0cfb25eaad6fc73d360dcab55a0097",
"assets/assets/svgs/chevronRight.svg": "de0fc39b167c61b1aec2dbf7c917de4a",
"assets/assets/svgs/printHistory.svg": "c2efa759db643e37210b23e2fc550392",
"assets/assets/svgs/deviceCardAdd.svg": "e3f9a614193c21a15f8f8d4bc4181adb",
"assets/assets/svgs/arrowDorpDown.svg": "b3758216634708055ebc74b438fed614",
"assets/assets/svgs/iconAuxiliaryCooling.svg": "c97707d78a2660a480f611fec1931f11",
"assets/assets/svgs/defaultAvatar.svg": "7c4c5e7ec2b7d53c14aabdb71c31d7a3",
"assets/assets/svgs/iconPurifier.svg": "b5b07b4c3cc3991141d8990051cb6969",
"assets/assets/svgs/iconWan.svg": "db72034317a6a8c9d65d7954404db9c6",
"assets/assets/svgs/iconLan.svg": "30d8e3e6074b7d99220cfb72b016bf8c",
"assets/assets/svgs/textfieldAccout.svg": "4324a5f9a2c0e966c77c6e8281274bf4",
"assets/assets/svgs/iconEdit.svg": "b492e4bd7293a12060506fdf19a94270",
"assets/assets/svgs/textfieldEye.svg": "3043be8436159cf8fd150e606c3ac89d",
"assets/assets/svgs/device/deviceActionHome.svg": "f25cb4b94d917b1feb29e300f87a7318",
"assets/assets/svgs/device/videoPlay.svg": "44a356f6cf6d1e9726efd3a121257427",
"assets/assets/svgs/device/iconSearch.svg": "ce74d3e650477cd96343e892a0749e95",
"assets/assets/svgs/device/iconFile.svg": "7218c79abf522375c54c4bfc5598b4dc",
"assets/assets/svgs/device/vector31.svg": "5cf210e843eeef5a3df7345ae1ebe1df",
"assets/assets/svgs/device/logout.svg": "f0deff29d0f3bf6a400aeba0527cb0b4",
"assets/assets/svgs/device/iconHome.svg": "7a33b9e84aad4afaff289d1b6c250408",
"assets/assets/svgs/device/exportFile.svg": "4e8f8088c9df3b0ca7d5bde427a87b0f",
"assets/assets/svgs/device/videoCall.svg": "0219d1249a7841f16206b3ea072a58cd",
"assets/assets/svgs/device/instructions.svg": "78dd949e4a6736bd3b7731600d67c10c",
"assets/assets/svgs/device/iconFan.svg": "af4af5acb2aee8e28956a4169a075abf",
"assets/assets/svgs/device/iconAdd.svg": "8f8a74c8bfcdc9dd86c9ecee88bf218b",
"assets/assets/svgs/device/keyboardArrowDropDown.svg": "d9323dc6b0866e4f56b7f4489813c76d",
"assets/assets/svgs/device/iconTick.svg": "a9c81d91bdf5edc7b59f93db23dfd9d5",
"assets/assets/svgs/device/iconTimeLapse.svg": "560db8226c70a89145231c0d76de8c32",
"assets/assets/svgs/device/deviceControl.svg": "53dfa32d8c5ba511476ec0828e029ec2",
"assets/assets/svgs/device/liveCamera.svg": "d5ec74d47b3cc05a517ad6d92a1afe73",
"assets/assets/svgs/device/settings.svg": "28c0bf1c19dd9f914ddd32006a2c5670",
"assets/assets/svgs/device/iconHotBedTemperature.svg": "042f68c8b72d9264f9486cf56badf75a",
"assets/assets/svgs/device/iconControlPause.svg": "21fa1a305b23c1e9f55533676d936c75",
"assets/assets/svgs/device/stop.svg": "708d85e998c9bf363daf6a372c8c065a",
"assets/assets/svgs/device/iconFilamentCheck.svg": "5841c886885ca2b500190f7111aafdcf",
"assets/assets/svgs/device/firmwareUpdate.svg": "11b2a4905d555eaa39daa41608d9619c",
"assets/assets/svgs/device/iconControlPlay.svg": "67d0740d95f0040bd7a0ca1b66ab8d85",
"assets/assets/svgs/device/wifi.svg": "baeb2c54264b71a2d1b3f85a573e6fa8",
"assets/assets/svgs/device/iconControlStop.svg": "1e51c70dfec7dc83a641dfdb2e51947c",
"assets/assets/svgs/device/iconMoreSetting.svg": "fedcf92ba2cc389fa8593569fa7dc55c",
"assets/assets/svgs/device/iconModelFileFolder.svg": "c1865cfda7a5a4d58127f8c574f62233",
"assets/assets/svgs/device/iconSpeed.svg": "245a121adf61c4199e4f034a64a83c1f",
"assets/assets/svgs/device/extruderBackground.svg": "b57537439ee6c33701b017ab217784b0",
"assets/assets/svgs/device/play.svg": "461a9e8a15d56121fc074c5b0dc13e21",
"assets/assets/svgs/device/exclamationMark.svg": "9c9276e8b725fed7847c664db6f6d46a",
"assets/assets/svgs/device/iconEdit.svg": "b9a168260cfab9604af94416634c23c0",
"assets/assets/svgs/device/iconExtruderHead.svg": "f21129c71a629449efc76b599eaaec6e",
"assets/assets/svgs/device/iconScan.svg": "a636a9fb0215d47b7ca5586518cc280a",
"assets/assets/svgs/device/iconBind.svg": "54afa4f31935ec0cc460debdadd28b9c",
"assets/assets/svgs/device/iconFilamentEdit.svg": "5f85342ed2b87be6fc33cc9efe577c45",
"assets/assets/svgs/device/addDevice.svg": "b405e1509086daf1b1eadbf7c700a9fe",
"assets/assets/svgs/device/delete.svg": "b720db40a7634e53b82ba4a935714b57",
"assets/assets/svgs/device/iconLed.svg": "f9d6f2b933e9c97c0efd459c5f77294d",
"assets/assets/svgs/device/pause.svg": "f47e268fb507c66aa3be1eebde72cf08",
"assets/assets/svgs/deviceCardLogout.svg": "2fce6860a04df4587e24d10d8142f587",
"assets/assets/svgs/iconArrowBack.svg": "49608b67ae21df82a430180d6415f14c",
"assets/assets/svgs/recentDocuments.svg": "073178369d3e83698fc454cf2ca524db",
"assets/assets/svgs/extruder/iconExtruder4.svg": "5053d1465dcddd849da42f368fd70161",
"assets/assets/svgs/extruder/iconExtruder1.svg": "6729d3e1ace84be33a63f400d7745f0b",
"assets/assets/svgs/extruder/iconExtruder2.svg": "0c2a2afd323d3cf42d44682537f88303",
"assets/assets/svgs/extruder/iconExtruder3.svg": "164862c8bb912a111e7219bddcfde6f3",
"assets/assets/svgs/loginPlatformGoogle.svg": "75211e661f2e76ca86ce3c0d3213cf60",
"assets/assets/svgs/cloud.svg": "85f1d05875666f8a5fdd65d5e943fe87",
"assets/assets/svgs/userIcon.svg": "1912f5002e86930f54cb592ec583617b",
"assets/assets/svgs/myDevice.svg": "eeec9fba44e96c7a39e4372d2bf7effe",
"assets/assets/files/accountDeletionAgreementZh.json": "b1545fcb09610595eec061d0ad907019",
"assets/assets/files/accountDeletionAgreement.json": "116a21e258107854e3502c8a34cd0f53",
"assets/assets/files/deviceError.json": "526e625b9f76cbd2bcbef6c3f03a5b56",
"assets/assets/fonts/HarmonyOS_Sans_SC_Regular.ttf": "872bdb620876bb570a7d5d9b9504abaf",
"assets/assets/fonts/NanumGothic-Regular.ttf": "3cbe9257e888255cb2dad0d8a80a7759",
"main.dart.js_2.part.js": "23525674fe4ea9a5a8cd9fe199f1d7a9",
"canvaskit/skwasm.js": "9fa2ffe90a40d062dd2343c7b84caf01",
"canvaskit/skwasm.js.symbols": "262f4827a1317abb59d71d6c587a93e2",
"canvaskit/canvaskit.js.symbols": "48c83a2ce573d9692e8d970e288d75f7",
"canvaskit/skwasm.wasm": "9f0c0c02b82a910d12ce0543ec130e60",
"canvaskit/chromium/canvaskit.js.symbols": "a012ed99ccba193cf96bb2643003f6fc",
"canvaskit/chromium/canvaskit.js": "87325e67bf77a9b483250e1fb1b54677",
"canvaskit/chromium/canvaskit.wasm": "b1ac05b29c127d86df4bcfbf50dd902a",
"canvaskit/canvaskit.js": "5fda3f1af7d6433d53b24083e2219fa0",
"canvaskit/canvaskit.wasm": "1f237a213d7370cf95f443d896176460",
"canvaskit/skwasm.worker.js": "bfb704a6c714a75da9ef320991e88b03"};
// The application shell files that are downloaded before a service worker can
// start.
const CORE = ["main.dart.js",
"index.html",
"flutter_bootstrap.js",
"assets/AssetManifest.bin.json",
"assets/FontManifest.json"];

// During install, the TEMP cache is populated with the application shell files.
self.addEventListener("install", (event) => {
  self.skipWaiting();
  return event.waitUntil(
    caches.open(TEMP).then((cache) => {
      return cache.addAll(
        CORE.map((value) => new Request(value, {'cache': 'reload'})));
    })
  );
});
// During activate, the cache is populated with the temp files downloaded in
// install. If this service worker is upgrading from one with a saved
// MANIFEST, then use this to retain unchanged resource files.
self.addEventListener("activate", function(event) {
  return event.waitUntil(async function() {
    try {
      var contentCache = await caches.open(CACHE_NAME);
      var tempCache = await caches.open(TEMP);
      var manifestCache = await caches.open(MANIFEST);
      var manifest = await manifestCache.match('manifest');
      // When there is no prior manifest, clear the entire cache.
      if (!manifest) {
        await caches.delete(CACHE_NAME);
        contentCache = await caches.open(CACHE_NAME);
        for (var request of await tempCache.keys()) {
          var response = await tempCache.match(request);
          await contentCache.put(request, response);
        }
        await caches.delete(TEMP);
        // Save the manifest to make future upgrades efficient.
        await manifestCache.put('manifest', new Response(JSON.stringify(RESOURCES)));
        // Claim client to enable caching on first launch
        self.clients.claim();
        return;
      }
      var oldManifest = await manifest.json();
      var origin = self.location.origin;
      for (var request of await contentCache.keys()) {
        var key = request.url.substring(origin.length + 1);
        if (key == "") {
          key = "/";
        }
        // If a resource from the old manifest is not in the new cache, or if
        // the MD5 sum has changed, delete it. Otherwise the resource is left
        // in the cache and can be reused by the new service worker.
        if (!RESOURCES[key] || RESOURCES[key] != oldManifest[key]) {
          await contentCache.delete(request);
        }
      }
      // Populate the cache with the app shell TEMP files, potentially overwriting
      // cache files preserved above.
      for (var request of await tempCache.keys()) {
        var response = await tempCache.match(request);
        await contentCache.put(request, response);
      }
      await caches.delete(TEMP);
      // Save the manifest to make future upgrades efficient.
      await manifestCache.put('manifest', new Response(JSON.stringify(RESOURCES)));
      // Claim client to enable caching on first launch
      self.clients.claim();
      return;
    } catch (err) {
      // On an unhandled exception the state of the cache cannot be guaranteed.
      console.error('Failed to upgrade service worker: ' + err);
      await caches.delete(CACHE_NAME);
      await caches.delete(TEMP);
      await caches.delete(MANIFEST);
    }
  }());
});
// The fetch handler redirects requests for RESOURCE files to the service
// worker cache.
self.addEventListener("fetch", (event) => {
  if (event.request.method !== 'GET') {
    return;
  }
  var origin = self.location.origin;
  var key = event.request.url.substring(origin.length + 1);
  // Redirect URLs to the index.html
  if (key.indexOf('?v=') != -1) {
    key = key.split('?v=')[0];
  }
  if (event.request.url == origin || event.request.url.startsWith(origin + '/#') || key == '') {
    key = '/';
  }
  // If the URL is not the RESOURCE list then return to signal that the
  // browser should take over.
  if (!RESOURCES[key]) {
    return;
  }
  // If the URL is the index.html, perform an online-first request.
  if (key == '/') {
    return onlineFirst(event);
  }
  event.respondWith(caches.open(CACHE_NAME)
    .then((cache) =>  {
      return cache.match(event.request).then((response) => {
        // Either respond with the cached resource, or perform a fetch and
        // lazily populate the cache only if the resource was successfully fetched.
        return response || fetch(event.request).then((response) => {
          if (response && Boolean(response.ok)) {
            cache.put(event.request, response.clone());
          }
          return response;
        });
      })
    })
  );
});
self.addEventListener('message', (event) => {
  // SkipWaiting can be used to immediately activate a waiting service worker.
  // This will also require a page refresh triggered by the main worker.
  if (event.data === 'skipWaiting') {
    self.skipWaiting();
    return;
  }
  if (event.data === 'downloadOffline') {
    downloadOffline();
    return;
  }
});
// Download offline will check the RESOURCES for all files not in the cache
// and populate them.
async function downloadOffline() {
  var resources = [];
  var contentCache = await caches.open(CACHE_NAME);
  var currentContent = {};
  for (var request of await contentCache.keys()) {
    var key = request.url.substring(origin.length + 1);
    if (key == "") {
      key = "/";
    }
    currentContent[key] = true;
  }
  for (var resourceKey of Object.keys(RESOURCES)) {
    if (!currentContent[resourceKey]) {
      resources.push(resourceKey);
    }
  }
  return contentCache.addAll(resources);
}
// Attempt to download the resource online before falling back to
// the offline cache.
function onlineFirst(event) {
  return event.respondWith(
    fetch(event.request).then((response) => {
      return caches.open(CACHE_NAME).then((cache) => {
        cache.put(event.request, response.clone());
        return response;
      });
    }).catch((error) => {
      return caches.open(CACHE_NAME).then((cache) => {
        return cache.match(event.request).then((response) => {
          if (response != null) {
            return response;
          }
          throw error;
        });
      });
    })
  );
}
