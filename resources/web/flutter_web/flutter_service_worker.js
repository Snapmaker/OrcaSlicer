'use strict';
const MANIFEST = 'flutter-app-manifest';
const TEMP = 'flutter-temp-cache';
const CACHE_NAME = 'flutter-app-cache';

const RESOURCES = {"flutter_bootstrap.js": "03dee884db2661ab332771f2410c6cb1",
"version.json": "88c3952ed6cb2d36c35bdd9951447c70",
"index.html": "381ad2cd57bb5159b93cf0cdd60e23ef",
"/": "381ad2cd57bb5159b93cf0cdd60e23ef",
"main.dart.js": "b07fc54d7ac3d1d4cb9f66ba830516b9",
"flutter.js": "f31737fb005cd3a3c6bd9355efd33061",
"favicon.png": "be8d1ab28c20907c9869c345d0482962",
"icons/Icon-192.png": "ab1f25ced1559729e334de938eae91a5",
"icons/Icon-maskable-192.png": "e41e8489c0f6a822acf8dab362e112b7",
"icons/Icon-maskable-512.png": "4870fb6720f4fcad016cb582589d136d",
"icons/Icon-512.png": "343022ac1c56796cb7ff635faf0646ef",
"manifest.json": "901d86fb8842ec0d66225a542131d689",
"assets/AssetManifest.json": "d5442c593c9f5283612d3dd5c7e34264",
"assets/NOTICES": "439ed4d5770b6e9a762356f44c07ce32",
"assets/FontManifest.json": "dc3d03800ccca4601324923c0b1d6d57",
"assets/AssetManifest.bin.json": "826b5f9b29617357bc43faa37cb6635a",
"assets/packages/cupertino_icons/assets/CupertinoIcons.ttf": "e986ebe42ef785b27164c36a9abc7818",
"assets/packages/flutter_inappwebview_web/assets/web/web_support.js": "509ae636cfdd93e49b5a6eaf0f06d79f",
"assets/packages/flutter_inappwebview/assets/t_rex_runner/t-rex.css": "5a8d0222407e388155d7d1395a75d5b9",
"assets/packages/flutter_inappwebview/assets/t_rex_runner/t-rex.html": "16911fcc170c8af1c5457940bd0bf055",
"assets/shaders/ink_sparkle.frag": "ecc85a2e95f5e9f53123dcaf8cb9b6ce",
"assets/AssetManifest.bin": "e53cdf3fd84944fbb4ea5aa038dd080e",
"assets/fonts/MaterialIcons-Regular.otf": "873bf4dcf4135f73e69d48c683c22dff",
"assets/assets/i10n/zh-CN.json": "14f55ba6b3ac37bc5ed2b8023599819c",
"assets/assets/i10n/en.json": "3798b9f81aed3fdb7b18129b5fc907e2",
"assets/assets/images/bind_printer.png": "b35f610897d7c4e891f3b0dc56d193f7",
"assets/assets/images/device_display_default.webp": "ea13b4cb58c0a38e8cf15f7033a07833",
"assets/assets/images/gcode_cover.png": "cd7e5c13429bc568b1c3ed8b3953ba86",
"assets/assets/images/3dprinter.png": "4515f02e633d58a1119c2f55114da32b",
"assets/assets/images/logo.png": "b1a7a2105873616de883cf2662a2bf9c",
"assets/assets/images/device/device_authorized.webp": "8eb814193bed15cec22658018871aba8",
"assets/assets/images/device/device_authorizing.webp": "ad1b45a33b70fe2e551a343cab248de3",
"assets/assets/images/device/device_search.webp": "86f0781f72a67ffdbedae3fddcae30f9",
"assets/assets/images/device/device_rejected.webp": "cb02f340100006ad22965c215fc6726c",
"assets/assets/images/default_empty.png": "a0cd09ca961ed194dae6b02cde0230b2",
"assets/assets/svgs/my_device.svg": "eeec9fba44e96c7a39e4372d2bf7effe",
"assets/assets/svgs/login_platform_apple.svg": "be43d78435feca50bbabad292a1039c7",
"assets/assets/svgs/textfield_eye.svg": "3043be8436159cf8fd150e606c3ac89d",
"assets/assets/svgs/recent_documents.svg": "073178369d3e83698fc454cf2ca524db",
"assets/assets/svgs/demo_warning.svg": "6ceb878e0b25c8cab6926cd13129af06",
"assets/assets/svgs/icon_arrow_back.svg": "49608b67ae21df82a430180d6415f14c",
"assets/assets/svgs/textfield_password.svg": "2e3a0e88318b56043a1b258032ee540c",
"assets/assets/svgs/login_platform_google.svg": "75211e661f2e76ca86ce3c0d3213cf60",
"assets/assets/svgs/print_history.svg": "c2efa759db643e37210b23e2fc550392",
"assets/assets/svgs/demo_no_device.svg": "33ff2df7ab748b6bbc27bd407e1d9eb8",
"assets/assets/svgs/tabbar/icon_discover_active.svg": "53ee419f392142d30b279754dc58ecf1",
"assets/assets/svgs/tabbar/icon_mine_active_dark.svg": "030b81bd2311b735940cfca9554ae04b",
"assets/assets/svgs/tabbar/icon_discover_active_dark.svg": "f36a009bc608d7ef47cfcc3b3aac0559",
"assets/assets/svgs/tabbar/icon_mine_active.svg": "d5e1793a5a31a4884d1ad0534b621735",
"assets/assets/svgs/tabbar/icon_discover_dark.svg": "3caaceb51949746c56ae5d5bc205c585",
"assets/assets/svgs/tabbar/icon_discover.svg": "291451c7c649c5966a00716a7233e1a5",
"assets/assets/svgs/tabbar/icon_service_active.svg": "0e09682f0c235f4d7b909fd3cf162891",
"assets/assets/svgs/tabbar/icon_mine_dark.svg": "0a440809fbab2570292432f4e9c950b1",
"assets/assets/svgs/tabbar/icon_service.svg": "2f49fff4b4ff0136667a32d6474ac6b9",
"assets/assets/svgs/tabbar/icon_device_active.svg": "76b799582124c5c5df23f3bfe83881bf",
"assets/assets/svgs/tabbar/icon_service_active_dark.svg": "5148985bca2986c5e07ba7940c10afd1",
"assets/assets/svgs/tabbar/icon_device.svg": "3a3acb433dd1e77848606e8ecf16c50d",
"assets/assets/svgs/tabbar/icon_device_active_dark.svg": "0733885cdc97289ca12341320862e9de",
"assets/assets/svgs/tabbar/icon_service_dark.svg": "d1273a072e99e19978ce169c51a06326",
"assets/assets/svgs/tabbar/icon_device_dark.svg": "1b3f24760bb5803737314cbc2cc6b6d1",
"assets/assets/svgs/tabbar/icon_mine.svg": "8c31cd203c81a95266cc30fe09937ed2",
"assets/assets/svgs/icon_close.svg": "f6db4c0e4369cc05ae28d3bea8d5b1ad",
"assets/assets/svgs/icon_edit.svg": "b9a168260cfab9604af94416634c23c0",
"assets/assets/svgs/default_avatar.svg": "7c4c5e7ec2b7d53c14aabdb71c31d7a3",
"assets/assets/svgs/device_card_add.svg": "e3f9a614193c21a15f8f8d4bc4181adb",
"assets/assets/svgs/icon_arrow_right.svg": "04a057ec0f5dbf3fa60be2625cc94379",
"assets/assets/svgs/arrow_dorp_down.svg": "b3758216634708055ebc74b438fed614",
"assets/assets/svgs/textfield_code.svg": "b0b13793929d8ea2daefea50788b0580",
"assets/assets/svgs/textfield_accout.svg": "4324a5f9a2c0e966c77c6e8281274bf4",
"assets/assets/svgs/device/icon_control_play.svg": "67d0740d95f0040bd7a0ca1b66ab8d85",
"assets/assets/svgs/device/icon_model_file_folder.svg": "c1865cfda7a5a4d58127f8c574f62233",
"assets/assets/svgs/device/icon_led.svg": "5f0ce6bc52f59cb0d38f31dcce29958a",
"assets/assets/svgs/device/icon_extruder_head.svg": "f21129c71a629449efc76b599eaaec6e",
"assets/assets/svgs/device/icon_scan.svg": "a636a9fb0215d47b7ca5586518cc280a",
"assets/assets/svgs/device/icon_filament_check.svg": "5841c886885ca2b500190f7111aafdcf",
"assets/assets/svgs/device/icon_bind.svg": "54afa4f31935ec0cc460debdadd28b9c",
"assets/assets/svgs/device/icon_search.svg": "ce74d3e650477cd96343e892a0749e95",
"assets/assets/svgs/device/keyboard_arrow_drop_down.svg": "d9323dc6b0866e4f56b7f4489813c76d",
"assets/assets/svgs/device/icon_filament_edit.svg": "5f85342ed2b87be6fc33cc9efe577c45",
"assets/assets/svgs/device/icon_edit.svg": "b9a168260cfab9604af94416634c23c0",
"assets/assets/svgs/device/extruder_background.svg": "b57537439ee6c33701b017ab217784b0",
"assets/assets/svgs/device/icon_control_pause.svg": "21fa1a305b23c1e9f55533676d936c75",
"assets/assets/svgs/device/icon_time_lapse.svg": "560db8226c70a89145231c0d76de8c32",
"assets/assets/svgs/device/icon_tick.svg": "a9c81d91bdf5edc7b59f93db23dfd9d5",
"assets/assets/svgs/device/icon_fan.svg": "e8bad0ebf6d42052de0df9e7d1a3ea6f",
"assets/assets/svgs/device/icon_add.svg": "8f8a74c8bfcdc9dd86c9ecee88bf218b",
"assets/assets/svgs/device/keyboard_arrow_drop_left.svg": "dbac249e48309d68b5f463d8e5f6d3a6",
"assets/assets/svgs/device/icon_home.svg": "7a33b9e84aad4afaff289d1b6c250408",
"assets/assets/svgs/device/icon_hot_bed_temperature.svg": "f769f54b30cb024e56f7bc2fd4336fd3",
"assets/assets/svgs/device/keyboard_arrow_drop_up.svg": "9804917021b693e2d56d27b41dc638d1",
"assets/assets/svgs/device/icon_speed.svg": "c061d4f8e95e2e7c4a4812baf58a72bd",
"assets/assets/svgs/device/keyboard_arrow_drop_right.svg": "6384cf21613d7557b0eac47c80ff49e2",
"assets/assets/svgs/device/icon_more_setting.svg": "fedcf92ba2cc389fa8593569fa7dc55c",
"assets/assets/svgs/device/icon_control_stop.svg": "1e51c70dfec7dc83a641dfdb2e51947c",
"assets/assets/svgs/login_platform_github.svg": "a3e7a4e22db16c9c9305b3c709c2d14c",
"assets/assets/svgs/login_platform_facebook.svg": "0a0cfb25eaad6fc73d360dcab55a0097",
"assets/assets/svgs/icon_notific.svg": "27082276596d830c36e1f5d0902b3929",
"assets/assets/svgs/cloud.svg": "85f1d05875666f8a5fdd65d5e943fe87",
"assets/assets/svgs/device_card_logout.svg": "2fce6860a04df4587e24d10d8142f587",
"assets/assets/svgs/demo_ic_printer_n1.svg": "45eed955f022e713aaf5667ca4292311",
"assets/assets/files/filament.json": "ccfd8eb9636c75a99b115059871cf862",
"assets/assets/files/device_error.json": "526e625b9f76cbd2bcbef6c3f03a5b56",
"assets/assets/files/account_deletion_agreement_zh.json": "b1545fcb09610595eec061d0ad907019",
"assets/assets/files/account_deletion_agreement.json": "116a21e258107854e3502c8a34cd0f53",
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
