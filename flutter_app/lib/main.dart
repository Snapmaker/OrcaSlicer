import 'dart:convert';
import 'dart:ffi';
import 'dart:ui';
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

// ═══════════════════════════════════════════════════════════════════
// Models
// ═══════════════════════════════════════════════════════════════════

class DeviceInfo {
  final String id, name;
  final String? ip, status;
  final bool connected;
  const DeviceInfo({required this.id, required this.name, this.ip, this.status, this.connected = false});
  factory DeviceInfo.fromJson(Map<String, dynamic> json) => DeviceInfo(
    id: json['id'], name: json['name'], ip: json['ip'], status: json['status'],
    connected: json['connected'] ?? false,
  );
}

class ScanParams {
  final String type;
  const ScanParams({required this.type});
  Map<String, dynamic> toJson() => {'type': type};
}

class ConnectParams {
  final String deviceId;
  final String? port;
  const ConnectParams({required this.deviceId, this.port});
  Map<String, dynamic> toJson() => {'deviceId': deviceId, if (port != null) 'port': port};
}

class PrinterInfo {
  final String id, name;
  final String? model, status;
  const PrinterInfo({required this.id, required this.name, this.model, this.status});
  factory PrinterInfo.fromJson(Map<String, dynamic> json) =>
      PrinterInfo(id: json['id'], name: json['name'], model: json['model'], status: json['status']);
}

class PrintFileInfo {
  final String fileName;
  final String? displayName;
  final double? estimatedTime, weight;
  const PrintFileInfo({required this.fileName, this.displayName, this.estimatedTime, this.weight});
  factory PrintFileInfo.fromJson(Map<String, dynamic> json) => PrintFileInfo(
    fileName: json['fileName'], displayName: json['displayName'],
    estimatedTime: (json['estimatedTime'] as num?)?.toDouble(),
    weight: (json['weight'] as num?)?.toDouble(),
  );
}

// ═══════════════════════════════════════════════════════════════════
// OrcaChannel
// ═══════════════════════════════════════════════════════════════════

class OrcaException implements Exception {
  final String code, message;
  OrcaException(this.code, this.message);
  @override String toString() => 'OrcaException($code): $message';
}

typedef _H = dynamic Function(String method, dynamic data);

class _Hub {
  final Map<String, _H> handlers = {};
  final List<_H> wildcards = [];
  bool mounted = false;
}

class OrcaChannel {
  final MethodChannel _channel;
  final String _name;
  final _Hub _hub;

  OrcaChannel(this._channel, [this._name = '']) : _hub = _Hub();
  OrcaChannel._child(this._channel, this._name, this._hub);

  String get name => _name;
  void cancel() { _channel.setMethodCallHandler(null); _hub.mounted = false; }
  void off(void Function(String method, dynamic data) handler) { _hub.wildcards.remove(handler); }
  OrcaChannel ns(String p) => OrcaChannel._child(_channel, _name.isEmpty ? p : '$_name.$p', _hub);
  String _fm(String m) => _name.isEmpty ? m : '$_name.$m';

  void on(String method, Future<dynamic> Function(dynamic data) handler) {
    _hub.handlers[_fm(method)] = (_, data) => handler(data);
    _mount();
  }

  void handle(void Function(String method, dynamic data) handler) {
    _hub.wildcards.add(handler);
    _mount();
  }

  void _mount() {
    if (_hub.mounted) return;
    _hub.mounted = true;
    Future.microtask(() => _channel.setMethodCallHandler((call) async {
      final data = _decode(call.arguments);
      final h = _hub.handlers[call.method];
      if (h != null) return h(call.method, data);
      for (final w in _hub.wildcards) {
        try { final r = w(call.method, data); if (r != null) return r; } catch (_) {}
      }
      return null;
    }));
  }

  Future<R> call<R>(String method, [dynamic args]) async {
    final payload = args == null ? null : (args is String ? args : jsonEncode(args));
    final resp = await _channel.invokeMethod<String>(_fm(method), payload);
    final j = jsonDecode(resp ?? '{}') as Map<String, dynamic>;
    if (j['ok'] != true) throw OrcaException(j['code'] ?? 'ERR', j['message'] ?? '');
    return j['data'] as R;
  }

  static dynamic _decode(dynamic arg) {
    if (arg is String) { try { return jsonDecode(arg); } catch (_) { return arg; } }
    return arg;
  }
}

// ═══════════════════════════════════════════════════════════════════
// Channel wrappers
// ═══════════════════════════════════════════════════════════════════

class DeviceChannel {
  final OrcaChannel _ch;
  DeviceChannel(OrcaChannel c) : _ch = c.ns('device');
  Future<List<DeviceInfo>> scan(String type) async {
    final l = await _ch.call<List>('scan', ScanParams(type: type).toJson());
    return (l).map((e) => DeviceInfo.fromJson(e as Map<String, dynamic>)).toList();
  }
  Future<void> connect(String id, {String? port}) =>
      _ch.call('connect', ConnectParams(deviceId: id, port: port).toJson());
  Future<void> disconnect() => _ch.call('disconnect');
  Future<Map<String, dynamic>> getStatus() => _ch.call<Map<String, dynamic>>('getStatus');
  void listen(void Function(String method, dynamic data) h) => _ch.handle(h);
  void off(void Function(String method, dynamic data) h) => _ch.off(h);
}

class PrinterChannel {
  final OrcaChannel _ch;
  PrinterChannel(OrcaChannel c) : _ch = c.ns('printer');
  Future<List<PrinterInfo>> getList() async {
    final l = await _ch.call<List>('getList');
    return (l).map((e) => PrinterInfo.fromJson(e as Map<String, dynamic>)).toList();
  }
  Future<void> select(String id) => _ch.call('select', {'printerId': id});
  Future<Map<String, dynamic>> getDetail(String id) =>
      _ch.call<Map<String, dynamic>>('getDetail', {'printerId': id});
  void listen(void Function(String method, dynamic data) h) => _ch.handle(h);
  void off(void Function(String method, dynamic data) h) => _ch.off(h);
}

class PrintChannel {
  final OrcaChannel _ch;
  PrintChannel(OrcaChannel c) : _ch = c.ns('print');
  Future<PrintFileInfo> getFileInfo() async {
    final d = await _ch.call<Map<String, dynamic>>('getFileInfo');
    return PrintFileInfo.fromJson(d);
  }
  Future<void> confirm() => _ch.call('confirm');
  Future<void> cancel() => _ch.call('cancel');
  void handle(void Function(String method, dynamic data) h) => _ch.handle(h);
}

// ═══════════════════════════════════════════════════════════════════
// Services
// ═══════════════════════════════════════════════════════════════════

class DeviceService extends ChangeNotifier {
  final DeviceChannel _c;
  List<DeviceInfo> _devices = [];
  String _status = 'idle';
  DeviceService(this._c) { _c.listen(_onPush); }
  List<DeviceInfo> get devices => List.unmodifiable(_devices);
  String get status => _status;
  bool get connected => _status != 'idle';
  Future<List<DeviceInfo>> scan(String type) async { _devices = await _c.scan(type); notifyListeners(); return _devices; }
  Future<void> connect(String id, {String? port}) async { await _c.connect(id, port: port); _status = 'connected'; notifyListeners(); }
  Future<void> disconnect() async { await _c.disconnect(); _status = 'idle'; notifyListeners(); }
  void _onPush(String m, dynamic d_) {
    final d = d_ as Map<String, dynamic>;
    switch (m) {
      case 'device.onStatusChanged': _status = d['status']; break;
      case 'device.onFound': _devices = [..._devices, DeviceInfo.fromJson(d)]; break;
      case 'device.onConnectionLost': _status = 'idle'; break;
    }
    notifyListeners();
  }
  @override void dispose() { _c.off(_onPush); super.dispose(); }
}

class PrinterService extends ChangeNotifier {
  final PrinterChannel _c;
  List<PrinterInfo> _printers = [];
  String? _selId;
  String _status = 'idle';
  PrinterService(this._c) { _c.listen(_onPush); }
  List<PrinterInfo> get printers => List.unmodifiable(_printers);
  String? get selectedPrinterId => _selId;
  String get status => _status;
  Future<void> loadPrinters() async { _printers = await _c.getList(); notifyListeners(); }
  Future<void> select(String id) async { await _c.select(id); _selId = id; notifyListeners(); }
  void _onPush(String m, dynamic d_) {
    if (m == 'printer.onStatusChanged') { _status = (d_ as Map)['status']; notifyListeners(); }
  }
  @override void dispose() { _c.off(_onPush); super.dispose(); }
}

// ═══════════════════════════════════════════════════════════════════
// Widgets
// ═══════════════════════════════════════════════════════════════════

class ErrorBoundary extends StatefulWidget {
  final Widget child;
  const ErrorBoundary({super.key, required this.child});
  @override State<ErrorBoundary> createState() => _ErrorBoundaryState();
}

class _ErrorBoundaryState extends State<ErrorBoundary> {
  Object? _err;
  int _ec = 0;
  DateTime? _first;
  void Function(FlutterErrorDetails)? _prev;
  bool get _open => _ec >= 3 && _first != null && DateTime.now().difference(_first!) < const Duration(seconds: 30);
  @override void initState() {
    super.initState();
    _prev = FlutterError.onError;
    FlutterError.onError = (d) { _record(d.exception, d.stack ?? StackTrace.empty); _prev?.call(d); };
  }
  void _reset() => setState(() { _err = null; _ec = 0; _first = null; });
  void _record(Object e, StackTrace s) { _err = e; _ec++; _first ??= DateTime.now(); setState(() {}); }
  @override void dispose() { FlutterError.onError = _prev; super.dispose(); }
  @override Widget build(BuildContext context) {
    if (_err != null && _open) return Center(child: Column(mainAxisSize: MainAxisSize.min, children: [
      const Icon(Icons.error_outline, size: 48, color: Colors.red), const SizedBox(height: 16),
      const Text('Too many errors', style: TextStyle(fontSize: 14)), const SizedBox(height: 8),
      FilledButton(onPressed: _reset, child: const Text('Reset')),
    ]));
    if (_err != null) return Center(child: Padding(padding: const EdgeInsets.all(24), child: Column(
      mainAxisSize: MainAxisSize.min, children: [
        const Icon(Icons.warning_amber, size: 48, color: Colors.orange), const SizedBox(height: 16),
        Text('$_err', style: const TextStyle(fontSize: 12), maxLines: 3), const SizedBox(height: 16),
        FilledButton(onPressed: _reset, child: const Text('Retry')),
      ])));
    return widget.child;
  }
}

class HomePage extends StatefulWidget {
  final DeviceService deviceService;
  final OrcaChannel channel;
  const HomePage({super.key, required this.deviceService, required this.channel});
  @override State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  String _ffi = '', _ping = '', _push = '';
  bool _busy = false;
  @override void initState() {
    super.initState();
    widget.channel.handle((m, d) { if (m == 'home.onPush' && mounted) setState(() => _push = (d is Map ? d['msg'] : d) as String); });
  }
  Future<void> _doFfi() async {
    setState(() { _busy = true; _ffi = '...'; });
    try {
      final d = await widget.channel.ns('home').call<Map<String, dynamic>>('getDemoData');
      // POC: FFI dereferences a raw C++ pointer. Safe only because the handler
      // callback is synchronous and the buffer (static std::string) is process-lifetime.
      final text = utf8.decode(Pointer<Uint8>.fromAddress(d['ptr'] as int).asTypedList(d['len'] as int));
      setState(() => _ffi = '${d["len"]}B, ${const LineSplitter().convert(text).length} lines OK');
    } catch (e) { setState(() => _ffi = 'Error: $e'); }
    setState(() => _busy = false);
  }
  Future<void> _doPing() async {
    setState(() { _busy = true; _ping = '...'; });
    try { final d = await widget.channel.ns('home').call<Map<String, dynamic>>('ping'); setState(() => _ping = 'Pong #${d["count"]} (${d["time"]}μs)'); }
    catch (e) { setState(() => _ping = 'Error: $e'); }
    setState(() => _busy = false);
  }
  Future<void> _doPush() async { setState(() => _push = 'waiting...'); await widget.channel.ns('home').call('requestPush'); }
  Widget _btn(String l, VoidCallback f) => FilledButton.tonal(onPressed: _busy ? null : f, child: Text(l));
  Widget _card(String t, String d, String r, Widget b) => Card(
    margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
    child: Padding(padding: const EdgeInsets.all(12), child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Text(t, style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 14)),
      Text(d, style: const TextStyle(color: Colors.grey, fontSize: 12)), const SizedBox(height: 8),
      if (r.isNotEmpty) Text(r, style: const TextStyle(fontFamily: 'monospace', fontSize: 13)), const SizedBox(height: 8), b,
    ])));
  @override Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Home')),
    body: Center(child: SizedBox(width: 520, child: Column(
      mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        _card('1. FFI Memory', 'C++ passes ptr+size, Dart reads via dart:ffi', _ffi, _btn('Read 5000 lines', _doFfi)),
        _card('2. Dart→C++ Ping', 'Dart calls C++ handler, C++ replies', _ping, _btn('Send Ping', _doPing)),
        _card('3. C++→Dart Push', 'C++ calls invoke, Dart handler receives', _push, _btn('Request Push', _doPush)),
      ]))));
}

class DevicePage extends StatelessWidget {
  final DeviceService deviceService;
  const DevicePage({super.key, required this.deviceService});
  @override Widget build(BuildContext context) => ListenableBuilder(listenable: deviceService, builder: (_, __) => Scaffold(
    appBar: AppBar(title: const Text('Device')),
    body: deviceService.devices.isEmpty
        ? const Center(child: Text('No devices', style: TextStyle(color: Colors.grey)))
        : ListView.builder(itemCount: deviceService.devices.length, itemBuilder: (_, i) {
            final d = deviceService.devices[i];
            return ListTile(
              leading: Icon(d.connected ? Icons.usb : Icons.devices, color: d.connected ? Colors.green : null),
              title: Text(d.name), subtitle: Text(d.ip ?? ''), trailing: d.connected ? const Chip(label: Text('Connected')) : null,
            );
          }),
  ));
}

class PreparePage extends StatelessWidget {
  const PreparePage({super.key});
  @override Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Prepare')),
    body: const Center(child: Column(mainAxisSize: MainAxisSize.min, children: [
      Icon(Icons.upload_file, size: 64, color: Colors.grey), SizedBox(height: 16),
      Text('Model preparation', style: TextStyle(color: Colors.grey, fontSize: 18)),
    ])),
  );
}

class PrintPage extends StatefulWidget {
  final PrinterService printerService;
  final PrintChannel printChannel;
  const PrintPage({super.key, required this.printerService, required this.printChannel});
  @override State<PrintPage> createState() => _PrintPageState();
}

class _PrintPageState extends State<PrintPage> {
  PrintFileInfo? _info;
  String? _sel;
  @override void initState() { super.initState(); _load(); }
  Future<void> _load() async {
    try { final i = await widget.printChannel.getFileInfo(); if (mounted) setState(() => _info = i); } catch (_) {}
  }
  @override Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('Print')),
    body: Padding(padding: const EdgeInsets.all(16), child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Card(child: Padding(padding: const EdgeInsets.all(12), child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text('File', style: Theme.of(context).textTheme.titleSmall), const SizedBox(height: 4),
        Text(_info?.displayName ?? _info?.fileName ?? 'Loading...', style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w500)),
        if (_info?.estimatedTime != null) ...[const SizedBox(height: 4), Text('Est. time: ${_info!.estimatedTime!.toStringAsFixed(0)} min', style: const TextStyle(color: Colors.grey))],
      ]))),
      const SizedBox(height: 12),
      ListenableBuilder(listenable: widget.printerService, builder: (_, __) {
        final ps = widget.printerService.printers;
        return Card(child: Padding(padding: const EdgeInsets.all(12), child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('Printer', style: Theme.of(context).textTheme.titleSmall), const SizedBox(height: 8),
          if (ps.isEmpty) const Text('No printers available', style: TextStyle(color: Colors.grey))
          else ...ps.map((p) => RadioListTile<String>(title: Text(p.name), subtitle: Text(p.model ?? ''), value: p.id, groupValue: _sel, onChanged: (v) => setState(() => _sel = v), contentPadding: EdgeInsets.zero, dense: true)),
        ])));
      }),
      const Spacer(),
    ])));
}

// ═══════════════════════════════════════════════════════════════════
// App entry
// ═══════════════════════════════════════════════════════════════════

class OrcaApp extends StatefulWidget {
  final OrcaChannel channel;
  final DeviceService deviceService;
  final PrinterService printerService;
  const OrcaApp({super.key, required this.channel, required this.deviceService, required this.printerService});
  @override State<OrcaApp> createState() => _OrcaAppState();
}

class _OrcaAppState extends State<OrcaApp> {
  int _idx = 0;
  static const _pages = ['home', 'device', 'prepare', 'print'];
  @override void initState() {
    super.initState();
    widget.channel.handle((m, d) {
      if (m == 'switchPage') { final i = _pages.indexOf(d is String ? d : (d as Map)['page'] as String); if (mounted && i >= 0) setState(() => _idx = i); }
    });
    widget.channel.ns('system').call('ready');
  }
  Widget _w(Widget c) => ErrorBoundary(child: c);
  @override Widget build(BuildContext context) => MaterialApp(
    debugShowCheckedModeBanner: false,
    theme: ThemeData(colorSchemeSeed: Colors.blue, useMaterial3: true),
    home: IndexedStack(index: _idx, children: [
      _w(HomePage(deviceService: widget.deviceService, channel: widget.channel)),
      _w(DevicePage(deviceService: widget.deviceService)),
      _w(const PreparePage()),
      _w(PrintPage(printerService: widget.printerService, printChannel: PrintChannel(widget.channel))),
    ]),
  );
}

@pragma('vm:entry-point')
void main() {
  PlatformDispatcher.instance.onError = (e, s) { debugPrint('[Orca] $e\n$s'); return true; };
  final c = OrcaChannel(const MethodChannel('snapmaker/orca'));
  runApp(OrcaApp(channel: c, deviceService: DeviceService(DeviceChannel(c)), printerService: PrinterService(PrinterChannel(c))));
}
